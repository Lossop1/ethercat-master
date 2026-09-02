#include "emaster/motion/relative_position.h"

#include <limits.h>
#include <string.h>

static uint64_t greatest_common_divisor(uint64_t left, uint64_t right)
{
    while (right != 0U)
    {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static bool multiply_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (result == NULL || (left != 0U && right > UINT64_MAX / left))
    {
        return false;
    }
    *result = left * right;
    return true;
}

/* 先约分再相乘，避免角度、编码器分辨率和减速比直接连乘造成静默溢出。 */
static bool angle_to_counts(uint32_t angle_millidegrees,
                            emaster_motion_coordinate_t coordinate,
                            const emaster_position_scale_t *scale,
                            uint64_t *counts)
{
    uint64_t numerators[3];
    uint64_t denominators[3];
    uint64_t numerator = 1U;
    uint64_t denominator = 1U;
    size_t numerator_index;
    size_t denominator_index;

    if (scale == NULL || counts == NULL || !scale->read_succeeded ||
        angle_millidegrees == 0U || scale->encoder_increments == 0U ||
        scale->encoder_motor_revolutions == 0U ||
        (coordinate != EMASTER_MOTION_COORDINATE_MOTOR_ROTOR &&
         coordinate != EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT) ||
        (coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT &&
         (scale->gear_motor_revolutions == 0U || scale->gear_shaft_revolutions == 0U)))
    {
        return false;
    }
    numerators[0] = angle_millidegrees;
    numerators[1] = scale->encoder_increments;
    numerators[2] = coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT
                        ? scale->gear_motor_revolutions
                        : UINT64_C(1);
    denominators[0] = UINT64_C(360000);
    denominators[1] = scale->encoder_motor_revolutions;
    denominators[2] = coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT
                          ? scale->gear_shaft_revolutions
                          : UINT64_C(1);

    for (numerator_index = 0U; numerator_index < 3U; ++numerator_index)
    {
        for (denominator_index = 0U; denominator_index < 3U; ++denominator_index)
        {
            uint64_t divisor = greatest_common_divisor(
                numerators[numerator_index], denominators[denominator_index]);
            numerators[numerator_index] /= divisor;
            denominators[denominator_index] /= divisor;
        }
    }
    for (numerator_index = 0U; numerator_index < 3U; ++numerator_index)
    {
        if (!multiply_u64(numerator, numerators[numerator_index], &numerator) ||
            !multiply_u64(denominator, denominators[numerator_index], &denominator))
        {
            return false;
        }
    }
    if (denominator == 0U || numerator > UINT64_MAX - denominator / UINT64_C(2))
    {
        return false;
    }
    *counts = (numerator + denominator / UINT64_C(2)) / denominator;
    return *counts != 0U;
}

static uint64_t absolute_difference_i32(int32_t left, int32_t right)
{
    int64_t difference = (int64_t)left - (int64_t)right;
    return (uint64_t)(difference < 0 ? -difference : difference);
}

emaster_relative_motion_status_t emaster_relative_motion_init(
    const emaster_motion_profile_t *profile,
    const emaster_motion_axis_config_t *const *axis_configs,
    const emaster_position_scale_t *scales,
    const int32_t *initial_positions,
    size_t axis_count,
    uint32_t cycle_ns,
    emaster_relative_motion_axis_t *axis_storage,
    emaster_relative_motion_t *motion)
{
    uint64_t duration_ns;
    uint64_t settle_ns;
    size_t axis_index;

    if (profile == NULL || profile->approval != EMASTER_MOTION_PROFILE_APPROVED ||
        profile->trajectory != EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION ||
        axis_configs == NULL || scales == NULL || initial_positions == NULL ||
        axis_storage == NULL || motion == NULL || axis_count == 0U ||
        profile->axis_count != axis_count || cycle_ns == 0U || profile->duration_ms == 0U)
    {
        return EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
    }
    duration_ns = (uint64_t)profile->duration_ms * UINT64_C(1000000);
    settle_ns = (uint64_t)profile->settle_ms * UINT64_C(1000000);
    memset(axis_storage, 0, axis_count * sizeof(*axis_storage));
    memset(motion, 0, sizeof(*motion));
    motion->duration_cycles =
        (duration_ns + (uint64_t)cycle_ns - UINT64_C(1)) / (uint64_t)cycle_ns;
    motion->settle_cycles =
        (settle_ns + (uint64_t)cycle_ns - UINT64_C(1)) / (uint64_t)cycle_ns;
    if (motion->duration_cycles == 0U)
    {
        return EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
    }

    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        const emaster_motion_axis_config_t *config = axis_configs[axis_index];
        emaster_relative_motion_axis_t *axis = &axis_storage[axis_index];
        uint32_t angle;
        uint64_t following_error_counts;
        int64_t final_position;

        if (config == NULL || config->relative_angle_millidegrees == 0 ||
            config->max_following_error_millidegrees == 0U)
        {
            return EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
        }
        angle = (uint32_t)(config->relative_angle_millidegrees < 0
                               ? -(int64_t)config->relative_angle_millidegrees
                               : config->relative_angle_millidegrees);
        if (!angle_to_counts(angle, profile->coordinate, &scales[axis_index],
                             &axis->delta_counts) ||
            !angle_to_counts(config->max_following_error_millidegrees,
                             profile->coordinate, &scales[axis_index],
                             &following_error_counts))
        {
            return EMASTER_RELATIVE_MOTION_INVALID_SCALE;
        }
        axis->direction = config->relative_angle_millidegrees < 0 ? INT8_C(-1) : INT8_C(1);
        axis->start_position = initial_positions[axis_index];
        axis->command_position = initial_positions[axis_index];
        axis->max_following_error_counts = following_error_counts;
        if (axis->delta_counts > (uint64_t)INT64_MAX)
        {
            return EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE;
        }
        final_position = (int64_t)axis->start_position +
                         (int64_t)axis->direction * (int64_t)axis->delta_counts;
        if (final_position < INT32_MIN ||
            final_position > INT32_MAX)
        {
            return EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE;
        }
        axis->final_position = (int32_t)final_position;
    }
    motion->profile = profile;
    motion->axes = axis_storage;
    motion->axis_count = axis_count;
    motion->initialized = true;
    return EMASTER_RELATIVE_MOTION_ACTIVE;
}

emaster_relative_motion_status_t emaster_relative_motion_step(
    emaster_relative_motion_t *motion,
    const int32_t *actual_positions,
    int32_t *target_positions,
    size_t axis_count)
{
    size_t axis_index;
    bool advanced_motion = false;

    if (motion == NULL || !motion->initialized || motion->axes == NULL ||
        actual_positions == NULL || target_positions == NULL ||
        axis_count != motion->axis_count)
    {
        return EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
    }
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        emaster_relative_motion_axis_t *axis = &motion->axes[axis_index];
        uint64_t following_error =
            absolute_difference_i32(actual_positions[axis_index], axis->command_position);

        if (following_error > axis->max_observed_following_error_counts)
        {
            axis->max_observed_following_error_counts = following_error;
        }
        if (following_error > axis->max_following_error_counts)
        {
            return EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR;
        }
    }

    if (motion->elapsed_motion_cycles < motion->duration_cycles)
    {
        advanced_motion = true;
        for (axis_index = 0U; axis_index < axis_count; ++axis_index)
        {
            emaster_relative_motion_axis_t *axis = &motion->axes[axis_index];
            uint64_t increment;
            int64_t next_position;

            axis->interpolation_remainder += axis->delta_counts;
            increment = axis->interpolation_remainder / motion->duration_cycles;
            axis->interpolation_remainder %= motion->duration_cycles;
            next_position = (int64_t)axis->command_position +
                            (int64_t)axis->direction * (int64_t)increment;
            if (next_position < INT32_MIN || next_position > INT32_MAX)
            {
                return EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE;
            }
            axis->command_position = (int32_t)next_position;
        }
        ++motion->elapsed_motion_cycles;
        if (motion->elapsed_motion_cycles == motion->duration_cycles)
        {
            for (axis_index = 0U; axis_index < axis_count; ++axis_index)
            {
                motion->axes[axis_index].command_position =
                    motion->axes[axis_index].final_position;
            }
        }
    }
    else if (motion->elapsed_settle_cycles < motion->settle_cycles)
    {
        ++motion->elapsed_settle_cycles;
    }

    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        target_positions[axis_index] = motion->axes[axis_index].command_position;
    }
    if (motion->elapsed_motion_cycles < motion->duration_cycles)
    {
        return EMASTER_RELATIVE_MOTION_ACTIVE;
    }
    if (advanced_motion)
    {
        return EMASTER_RELATIVE_MOTION_ACTIVE;
    }
    if (motion->elapsed_settle_cycles < motion->settle_cycles)
    {
        return EMASTER_RELATIVE_MOTION_SETTLING;
    }
    return EMASTER_RELATIVE_MOTION_COMPLETE;
}
