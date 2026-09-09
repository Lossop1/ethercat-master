#include "emaster/motion/position_target.h"

#include <limits.h>

static bool model_is_valid(const emaster_position_target_axis_model_t *model)
{
    if (model == NULL ||
        (model->polarity != INT8_C(1) && model->polarity != INT8_C(-1)) ||
        (model->coordinate != EMASTER_MOTION_COORDINATE_MOTOR_ROTOR &&
         model->coordinate != EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT) ||
        !model->scale.read_succeeded || model->scale.encoder_increments == 0U ||
        model->scale.encoder_motor_revolutions == 0U)
    {
        return false;
    }
    if (model->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT &&
        (model->scale.gear_motor_revolutions == 0U ||
         model->scale.gear_shaft_revolutions == 0U))
    {
        return false;
    }
    return !model->limits_enabled || model->minimum_counts <= model->maximum_counts;
}

emaster_position_target_status_t emaster_position_target_from_angle(
    const emaster_position_target_axis_model_t *model,
    emaster_position_target_kind_t kind,
    int32_t reference_counts,
    int32_t angle_millidegrees,
    int32_t *target_counts)
{
    int64_t angle_counts;
    int64_t target;

    if (model == NULL || target_counts == NULL ||
        (kind != EMASTER_POSITION_TARGET_RELATIVE &&
         kind != EMASTER_POSITION_TARGET_ABSOLUTE))
    {
        return EMASTER_POSITION_TARGET_INVALID_ARGUMENT;
    }
    if (!model->scale.read_succeeded)
    {
        return EMASTER_POSITION_TARGET_INVALID_SCALE;
    }
    if (!model_is_valid(model))
    {
        return EMASTER_POSITION_TARGET_INVALID_ARGUMENT;
    }
    if (!emaster_motion_angle_to_counts(
            angle_millidegrees, model->coordinate, &model->scale, &angle_counts))
    {
        return EMASTER_POSITION_TARGET_OUT_OF_RANGE;
    }
    angle_counts *= (int64_t)model->polarity;
    target = kind == EMASTER_POSITION_TARGET_ABSOLUTE
                 ? (int64_t)model->absolute_zero_counts + angle_counts
                 : (int64_t)reference_counts + angle_counts;
    if (target < INT32_MIN || target > INT32_MAX)
    {
        return EMASTER_POSITION_TARGET_OUT_OF_RANGE;
    }
    if (model->limits_enabled &&
        (target < model->minimum_counts || target > model->maximum_counts))
    {
        return EMASTER_POSITION_TARGET_LIMIT_VIOLATION;
    }
    *target_counts = (int32_t)target;
    return EMASTER_POSITION_TARGET_OK;
}
