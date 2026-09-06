#include "session_internal.h"

#include <string.h>

void emaster_soem_session_note_runtime_failure(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status,
    emaster_control_session_status_t *first_status)
{
    emaster_soem_session_latch_failure(session, status);
    if (first_status != NULL && *first_status == EMASTER_CONTROL_SESSION_OK)
    {
        *first_status = status;
    }
}

emaster_control_session_status_t emaster_soem_session_publish_feedback(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status)
{
    if (session != NULL && session->feedback_updated != NULL)
    {
        const emaster_control_feedback_frame_t feedback = {
            session->report->cycle_count,
            session->report->state,
            status,
            session->report->fault_latched,
            session->report->safety_control_permitted,
            session->report->safety_blocking_reasons,
            session->axes,
            session->plan->axis_count};

        session->feedback_updated(&feedback, session->feedback_user_data);
    }
    return status;
}

static void collect_safety_conditions(
    emaster_soem_session_t *session,
    bool feedback_valid,
    bool all_modes_confirmed,
    emaster_control_session_status_t status,
    bool stop_requested,
    emaster_safety_conditions_t *conditions)
{
    size_t axis_index;

    memset(conditions, 0, sizeof(*conditions));
    conditions->topology_verified = true;
    conditions->pdo_verified = true;
    conditions->output_initialized = true;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        conditions->topology_verified &= session->axes[axis_index].identity_match;
        conditions->pdo_verified &= session->axes[axis_index].pdo_match &&
                                    session->axes[axis_index].process_map_match;
        conditions->output_initialized &= session->axes[axis_index].output_initialized;
    }
    conditions->communication_healthy = true;
    conditions->command_valid = session->plan->motion_profile == NULL ||
                                session->plan->motion_profile->approval ==
                                    EMASTER_MOTION_PROFILE_APPROVED;
    conditions->feedback_valid = feedback_valid;
    conditions->mode_confirmed = all_modes_confirmed;
    conditions->synchronization_healthy = !session->dc_required ||
                                          session->clock.dc_feedback_valid;
    conditions->target_valid =
        (session->plan->motion_profile == NULL || session->motion_prepared) &&
        status != EMASTER_CONTROL_SESSION_MOTION_INVALID &&
        status != EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
    conditions->drive_limit_inactive =
        status != EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE;
    conditions->enable_authorized = !session->fault_latched;
    conditions->stop_requested = stop_requested;
    conditions->fault_latched = session->fault_latched;
}

static bool set_all_goals(emaster_soem_session_t *session,
                          emaster_cia402_goal_t goal,
                          bool calculate_safe_output)
{
    size_t axis_index;

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (!emaster_cia402_controller_set_goal(&session->controllers[axis_index], goal))
        {
            return false;
        }
        if (calculate_safe_output &&
            !emaster_cia402_controller_step(
                &session->controllers[axis_index], session->status_words[axis_index],
                &session->controller_outputs[axis_index]))
        {
            return false;
        }
    }
    return true;
}

emaster_control_session_status_t emaster_soem_session_apply_safety(
    emaster_soem_session_t *session,
    bool feedback_valid,
    bool all_modes_confirmed,
    emaster_control_session_status_t current_status,
    bool *denied)
{
    emaster_safety_conditions_t conditions;
    emaster_safety_decision_t decision;
    bool stop_requested;

    if (session == NULL || session->plan == NULL || session->report == NULL ||
        denied == NULL)
    {
        return EMASTER_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    stop_requested = session->stop_requested != NULL &&
                     session->stop_requested(session->stop_user_data);
    collect_safety_conditions(session, feedback_valid, all_modes_confirmed,
                              current_status, stop_requested, &conditions);
    if (!emaster_safety_evaluate(&conditions, &decision))
    {
        emaster_soem_session_note_runtime_failure(
            session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED, &current_status);
        *denied = true;
        return current_status;
    }
    session->report->safety_blocking_reasons = decision.blocking_reasons;
    session->report->safety_control_permitted = decision.control_permitted;
    *denied = !decision.control_permitted;
    if (decision.control_permitted)
    {
        if (!set_all_goals(session, EMASTER_CIA402_GOAL_OPERATION_ENABLED, false))
        {
            emaster_soem_session_note_runtime_failure(
                session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED, &current_status);
            *denied = true;
        }
        return current_status;
    }
    if (!set_all_goals(session, EMASTER_CIA402_GOAL_SAFE_STOP, true))
    {
        emaster_soem_session_note_runtime_failure(
            session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED, &current_status);
        return current_status;
    }
    if (stop_requested)
    {
        session->report->stop_requested = true;
        return session->fault_latched ? current_status : EMASTER_CONTROL_SESSION_OK;
    }
    if (!feedback_valid && current_status == EMASTER_CONTROL_SESSION_OK)
    {
        emaster_soem_session_note_runtime_failure(
            session, EMASTER_CONTROL_SESSION_FEEDBACK_INVALID, &current_status);
    }
    return current_status;
}
