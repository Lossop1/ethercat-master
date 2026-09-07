#include "emaster/motion/velocity_profile.h"

#include <limits.h>
#include <string.h>

static uint64_t abs_i32(int32_t value)
{
    int64_t wide = value;
    return (uint64_t)(wide < 0 ? -wide : wide);
}

static bool multiply_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (result == NULL || (left != 0U && right > UINT64_MAX / left)) return false;
    *result = left * right;
    return true;
}

static bool velocity_to_counts(int32_t millidegrees_per_second,
                               const emaster_motion_profile_t *profile,
                               const emaster_position_scale_t *scale,
                               int32_t *result)
{
    uint64_t magnitude;
    uint64_t numerator;
    uint64_t denominator;
    bool negative;

    if (profile == NULL || scale == NULL || result == NULL || !scale->read_succeeded ||
        scale->encoder_increments == 0U || scale->encoder_motor_revolutions == 0U ||
        (profile->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT &&
            (scale->gear_motor_revolutions == 0U || scale->gear_shaft_revolutions == 0U)))
        return false;
    negative = millidegrees_per_second < 0;
    magnitude = abs_i32(millidegrees_per_second);
    numerator = magnitude;
    if (!multiply_u64(numerator, scale->encoder_increments, &numerator)) return false;
    denominator = UINT64_C(360000) * scale->encoder_motor_revolutions;
    if (profile->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT)
    {
        if (!multiply_u64(numerator, scale->gear_motor_revolutions, &numerator) ||
            !multiply_u64(denominator, scale->gear_shaft_revolutions, &denominator))
            return false;
    }
    if (denominator == 0U || numerator > UINT64_MAX - denominator / 2U) return false;
    magnitude = (numerator + denominator / 2U) / denominator;
    if (magnitude > (uint64_t)INT32_MAX + (negative ? UINT64_C(1) : UINT64_C(0))) return false;
    if (negative)
    {
        *result = magnitude == UINT64_C(1) << 31 ? INT32_MIN : -(int32_t)magnitude;
    }
    else *result = (int32_t)magnitude;
    return true;
}

static bool rate_to_counts(uint32_t millidegrees_per_second2,
                           const emaster_motion_profile_t *profile,
                           const emaster_position_scale_t *scale,
                           uint64_t *result)
{
    uint64_t numerator;
    uint64_t denominator;
    if (profile == NULL || scale == NULL || result == NULL || !scale->read_succeeded ||
        scale->encoder_increments == 0U || scale->encoder_motor_revolutions == 0U ||
        (profile->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT &&
         (scale->gear_motor_revolutions == 0U || scale->gear_shaft_revolutions == 0U)))
        return false;
    numerator = millidegrees_per_second2;
    if (!multiply_u64(numerator, scale->encoder_increments, &numerator) ||
        !multiply_u64(numerator, profile->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT
                                  ? scale->gear_motor_revolutions : UINT64_C(1), &numerator))
        return false;
    denominator = UINT64_C(360000) * scale->encoder_motor_revolutions;
    if (!multiply_u64(denominator, profile->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT
                                    ? scale->gear_shaft_revolutions : UINT64_C(1), &denominator) ||
        denominator == 0U || numerator > UINT64_MAX - denominator / 2U)
        return false;
    *result = (numerator + denominator / 2U) / denominator;
    return true;
}

bool emaster_velocity_motion_init(const emaster_motion_profile_t *profile,
                                  const emaster_motion_axis_config_t *const *axis_configs,
                                  const emaster_position_scale_t *scales,
                                  size_t axis_count, uint32_t cycle_ns,
                                  emaster_velocity_axis_t *axis_storage,
                                  emaster_velocity_motion_t *motion)
{
    size_t index;
    if (profile == NULL || profile->approval != EMASTER_MOTION_PROFILE_APPROVED ||
        profile->trajectory != EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY ||
        axis_configs == NULL || scales == NULL || axis_storage == NULL || motion == NULL ||
        axis_count == 0U || profile->axis_count != axis_count || cycle_ns == 0U ||
        profile->duration_ms == 0U)
        return false;
    memset(axis_storage, 0, axis_count * sizeof(*axis_storage));
    memset(motion, 0, sizeof(*motion));
    motion->duration_cycles = ((uint64_t)profile->duration_ms * 1000000U + cycle_ns - 1U) / cycle_ns;
    motion->settle_cycles = ((uint64_t)profile->settle_ms * 1000000U + cycle_ns - 1U) / cycle_ns;
    motion->axes = axis_storage;
    motion->axis_count = axis_count;
    motion->cycle_ns = cycle_ns;
    for (index = 0U; index < axis_count; ++index)
    {
        emaster_velocity_axis_t *axis = &axis_storage[index];
        const emaster_motion_axis_config_t *config = axis_configs[index];
        int32_t target;
        if (config == NULL || config->max_velocity_error_millidegrees_per_second == 0U ||
            !velocity_to_counts(config->target_velocity_millidegrees_per_second, profile,
                                &scales[index], &target) ||
            !rate_to_counts(config->acceleration_millidegrees_per_second2, profile,
                            &scales[index], &axis->acceleration) ||
            !rate_to_counts(config->deceleration_millidegrees_per_second2, profile,
                            &scales[index], &axis->deceleration) ||
            !rate_to_counts(config->max_velocity_error_millidegrees_per_second, profile,
                            &scales[index], &axis->max_velocity_error))
            return false;
        if (axis->acceleration == 0U || axis->deceleration == 0U) return false;
        axis->target_velocity = target;
    }
    motion->initialized = true;
    return true;
}

emaster_relative_motion_status_t emaster_velocity_motion_step(
    emaster_velocity_motion_t *motion, const int32_t *actual_velocities,
    int32_t *target_velocities, size_t axis_count)
{
    size_t index;
    bool stopping;
    if (motion == NULL || !motion->initialized || actual_velocities == NULL ||
        target_velocities == NULL || axis_count != motion->axis_count)
        return EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
    stopping = motion->elapsed_cycles >= motion->duration_cycles;
    for (index = 0U; index < axis_count; ++index)
    {
        emaster_velocity_axis_t *axis = &motion->axes[index];
        int64_t difference = (int64_t)actual_velocities[index] - axis->command_velocity;
        uint64_t error = (uint64_t)(difference < 0 ? -difference : difference);
        uint64_t rate = stopping ? axis->deceleration : axis->acceleration;
        uint64_t rate_numerator;
        uint64_t increment;
        int64_t next = axis->command_velocity;
        if (rate > UINT64_MAX / motion->cycle_ns ||
            rate * motion->cycle_ns > UINT64_MAX - axis->acceleration_remainder)
            return EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE;
        rate_numerator = rate * motion->cycle_ns + axis->acceleration_remainder;
        increment = rate_numerator / UINT64_C(1000000000);
        axis->acceleration_remainder = rate_numerator % UINT64_C(1000000000);
        if (increment == 0U && rate != 0U && !stopping && next != axis->target_velocity)
            increment = 1U;
        if (error > axis->max_velocity_error) return EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR;
        if (stopping)
        {
            if (next > 0) next = next > (int64_t)increment ? next - (int64_t)increment : 0;
            else if (next < 0) next = next < -(int64_t)increment ? next + (int64_t)increment : 0;
        }
        else if (next < axis->target_velocity)
        {
            next += (int64_t)increment;
            if (next > axis->target_velocity) next = axis->target_velocity;
        }
        else if (next > axis->target_velocity)
        {
            next -= (int64_t)increment;
            if (next < axis->target_velocity) next = axis->target_velocity;
        }
        if (next < INT32_MIN || next > INT32_MAX) return EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE;
        axis->command_velocity = (int32_t)next;
        target_velocities[index] = axis->command_velocity;
    }
    if (!stopping) ++motion->elapsed_cycles;
    else if (motion->elapsed_settle_cycles < motion->settle_cycles) ++motion->elapsed_settle_cycles;
    if (!stopping) return EMASTER_RELATIVE_MOTION_ACTIVE;
    for (index = 0U; index < axis_count; ++index)
        if (motion->axes[index].command_velocity != 0) return EMASTER_RELATIVE_MOTION_SETTLING;
    return motion->elapsed_settle_cycles < motion->settle_cycles
               ? EMASTER_RELATIVE_MOTION_SETTLING : EMASTER_RELATIVE_MOTION_COMPLETE;
}
