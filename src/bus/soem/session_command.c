#include "session_internal.h"

#include <string.h>

static bool target_within_limits(
    const emaster_control_session_axis_result_t *axis,
    int32_t target)
{
    if (axis == NULL || !axis->software_position_limits_read)
    {
        return true;
    }
    /* 最小值等于最大值时，设备没有提供可用的位置区间。 */
    if (axis->software_position_limit_min == axis->software_position_limit_max)
    {
        return true;
    }
    return target >= axis->software_position_limit_min &&
           target <= axis->software_position_limit_max;
}

emaster_control_session_status_t emaster_soem_session_position_command_step(
    emaster_soem_session_t *session,
    bool *updated)
{
    emaster_position_command_observation_t observation;
    emaster_position_command_status_t command_status;
    size_t axis_index;

    if (session == NULL || session->plan == NULL || session->report == NULL ||
        session->position_command == NULL || session->position_command_targets == NULL ||
        updated == NULL || session->plan->motion_profile == NULL ||
        session->plan->motion_profile->trajectory !=
            EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION)
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
    memset(&observation, 0, sizeof(observation));
    command_status = session->position_command(
        session->report->cycle_count, session->axes, session->plan->axis_count,
        session->position_command_targets, session->plan->axis_count,
        &observation, session->position_command_user_data);
    if (command_status == EMASTER_POSITION_COMMAND_NO_COMMAND ||
        command_status == EMASTER_POSITION_COMMAND_HOLD)
    {
        if (session->position_command_active &&
            !emaster_position_command_is_active(
                session->report->cycle_count, session->position_command_expire_cycle))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
        return EMASTER_CONTROL_SESSION_OK;
    }
    if (command_status != EMASTER_POSITION_COMMAND_UPDATED ||
        observation.sequence == 0U ||
        observation.sequence <= session->position_command_sequence ||
        observation.activate_cycle > session->report->cycle_count ||
        observation.expire_cycle <= session->report->cycle_count)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (!target_within_limits(&session->axes[axis_index],
                                  session->position_command_targets[axis_index]))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }
    memcpy(session->target_positions, session->position_command_targets,
           session->plan->axis_count * sizeof(*session->target_positions));
    session->position_command_sequence = observation.sequence;
    session->position_command_expire_cycle = observation.expire_cycle;
    session->position_command_active = true;
    *updated = true;
    return EMASTER_CONTROL_SESSION_OK;
}
