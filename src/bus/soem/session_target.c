#include "session_internal.h"

#include <string.h>

static bool target_within_limits(
    const emaster_control_session_axis_result_t *axis,
    int32_t target)
{
    if (axis == NULL || !axis->software_position_limits_read)
    {
        return false;
    }
    /* 最小值等于最大值时，设备没有提供可用的位置区间。 */
    if (axis->software_position_limit_min == axis->software_position_limit_max)
    {
        return true;
    }
    return target >= axis->software_position_limit_min &&
           target <= axis->software_position_limit_max;
}

emaster_control_session_status_t emaster_soem_session_position_target_step(
    emaster_soem_session_t *session,
    bool *updated)
{
    emaster_position_target_source_result_t source_result;
    size_t axis_index;

    if (session == NULL || session->plan == NULL || session->report == NULL ||
        session->position_target_source == NULL ||
        session->position_target_source_targets == NULL ||
        updated == NULL || session->plan->axes == NULL)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }
    *updated = false;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (session->plan->axes[axis_index].operation_mode == NULL ||
            session->plan->axes[axis_index].operation_mode->value != INT8_C(8))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }
    source_result = session->position_target_source(
        session->report->cycle_count, session->axes, session->plan->axis_count,
        session->position_target_source_targets, session->plan->axis_count,
        session->position_target_source_user_data);
    if (source_result == EMASTER_POSITION_TARGET_SOURCE_HOLD)
    {
        return EMASTER_CONTROL_SESSION_OK;
    }
    if (source_result != EMASTER_POSITION_TARGET_SOURCE_UPDATED)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (!target_within_limits(&session->axes[axis_index],
                                  session->position_target_source_targets[axis_index]))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }
    memcpy(session->target_positions, session->position_target_source_targets,
           session->plan->axis_count * sizeof(*session->target_positions));
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (!emaster_soem_axis_set_target_value(
                &session->plan->axes[axis_index], &session->axes[axis_index],
                session->position_target_source_targets[axis_index]))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }
    *updated = true;
    return EMASTER_CONTROL_SESSION_OK;
}
