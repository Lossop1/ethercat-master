#include "session_internal.h"

#include <limits.h>

static uint64_t absolute_position_difference(int32_t left, int32_t right) {
    int64_t difference = (int64_t)left - (int64_t)right;

    return (uint64_t)(difference < 0 ? -difference : difference);
}

emaster_control_session_status_t emaster_soem_session_run(emaster_soem_session_t *session) {
    size_t axis_index;
    bool motion_initialized = false;
    bool enable_started = false;
    uint64_t mode_confirmation_deadline_cycle;
    uint64_t enable_deadline_cycle = 0U;
    emaster_control_session_status_t status = EMASTER_CONTROL_SESSION_OK;
    emaster_control_session_status_t safety_status = EMASTER_CONTROL_SESSION_OK;

    emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_ENABLING,
                                   EMASTER_CONTROL_SESSION_OK);

    mode_confirmation_deadline_cycle =
        session->report->cycle_count + session->transition_cycles;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        if (!emaster_cia_process_image_decode_input(&session->images[axis_index], slave->inputs,
                                                    slave->Ibytes, &mode_display, &status_word,
                                                    &actual_position) ||
            !emaster_cia_process_image_audit_input(
                &session->images[axis_index], &session->report->audit,
                EMASTER_AUDIT_PHASE_OPERATION_REQUEST, (uint16_t)(axis_index + 1U),
                session->exchange)) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }
    {
        while (true) {
            emaster_multiaxis_frame_t frame;
            uint64_t now_ns;
            uint64_t deadline_ns;
            bool all_axes_enabled = true;
            bool all_modes_confirmed = true;
            bool feedback_valid = true;
            bool safety_denied = false;

            status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
                int8_t mode_display;
                uint16_t status_word;
                int32_t actual_position;

                session->axes[axis_index].input_decoded = emaster_cia_process_image_decode_input(
                    &session->images[axis_index], slave->inputs, slave->Ibytes, &mode_display,
                    &status_word, &actual_position);
                if (!session->axes[axis_index].input_decoded) {
                    feedback_valid = false;
                }
                if (!session->axes[axis_index].input_decoded)
                {
                    continue;
                }
                if (!emaster_cia_process_image_audit_input(
                        &session->images[axis_index], &session->report->audit,
                        EMASTER_AUDIT_PHASE_CYCLIC_OPERATION, (uint16_t)(axis_index + 1U),
                        session->exchange)) {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    return status;
                }
                session->axes[axis_index].mode_display = mode_display;
                session->axes[axis_index].status_word = status_word;
                if (!emaster_soem_axis_set_feedback(&session->plan->axes[axis_index],
                                                    &session->axes[axis_index], actual_position))
                    feedback_valid = false;
                session->status_words[axis_index] = status_word;
                {
                    emaster_cia402_status_t decoded_status;

                    (void)emaster_cia402_decode_status(status_word, &decoded_status);
                    session->axes[axis_index].status_warning = decoded_status.warning;
                    session->axes[axis_index].voltage_enabled = decoded_status.voltage_enabled;
                    session->axes[axis_index].remote = decoded_status.remote;
                    session->axes[axis_index].target_reached = decoded_status.target_reached;
                    session->axes[axis_index].internal_limit_active =
                        decoded_status.internal_limit_active;
                    session->axes[axis_index].mode_specific_status =
                        decoded_status.mode_specific_bits;
                    session->axes[axis_index].manufacturer_specific_status =
                        decoded_status.manufacturer_specific_bits;
                }
                if (session->plan->motion_profile != NULL) {
                    session->actual_positions[axis_index] = actual_position;
                }
            }

            if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &deadline_ns)) {
                return session->clock.deadline_missed
                           ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                           : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;
            }
            frame.sequence = session->report->cycle_count;
            frame.deadline_ns = deadline_ns;
            frame.axis_count = session->plan->axis_count;
            frame.status_words = session->status_words;
            frame.outputs = session->controller_outputs;
            if (emaster_multiaxis_coordinator_step(&session->coordinator, &frame, now_ns) !=
                EMASTER_MULTIAXIS_OK) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_latch_failure(session, status);
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];
                bool operation_enabled = session->controller_outputs[axis_index].observed_state ==
                                         EMASTER_CIA402_STATE_OPERATION_ENABLED;
                bool mode_control_ready;

                axis_result->control_word = session->controller_outputs[axis_index].control_word;
                axis_result->cia402_state = session->controller_outputs[axis_index].observed_state;
                axis_result->operation_enabled_seen |= operation_enabled;
                axis_result->switch_on_disabled_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCH_ON_DISABLED;
                axis_result->ready_to_switch_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
                axis_result->switched_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCHED_ON;
                if (!session->images[axis_index].tx_mode_available &&
                    axis_result->mode_display_sdo_read) {
                    axis_result->mode_display = axis_result->mode_display_sdo;
                }
                axis_result->mode_display_match =
                    session->images[axis_index].tx_mode_available
                        ? axis_result->mode_display ==
                              session->plan->axes[axis_index].operation_mode->value
                        : axis_result->mode_display_sdo_read &&
                              axis_result->mode_display ==
                                  session->plan->axes[axis_index].operation_mode->value;
                mode_control_ready = axis_result->mode_display_match;
                if (!session->images[axis_index].tx_mode_available)
                {
                    /* 固定 PDO 无法提供周期 6061，只能以 SAFE-OP 的 6060 写后读回作为使能前证据 */
                    mode_control_ready = axis_result->mode_command_sdo_read &&
                        axis_result->mode_command_sdo ==
                            session->plan->axes[axis_index].operation_mode->value;
                }
                if (!emaster_session_axis_mode_allows_control(&session->plan->axes[axis_index],
                                                              mode_control_ready)) {
                    all_modes_confirmed = false;
                }
                if (session->controller_outputs[axis_index].fault_present) {
                    status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                if (!session->controller_outputs[axis_index].state_known)
                {
                    emaster_soem_session_note_runtime_failure(session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
                                         &safety_status);
                    safety_denied = true;
                }
                if (axis_result->internal_limit_active)
                {
                    emaster_soem_session_note_runtime_failure(session,
                                         EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE,
                                         &safety_status);
                    safety_denied = true;
                }
                if (!operation_enabled) {
                    all_axes_enabled = false;
                }
                else if (!axis_result->voltage_enabled)
                {
                    emaster_soem_session_note_runtime_failure(session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
                                         &safety_status);
                    safety_denied = true;
                }
            }
            session->report->all_axes_enabled_reached |= all_axes_enabled;
            if (all_modes_confirmed && !enable_started)
            {
                enable_started = true;
                enable_deadline_cycle =
                    session->report->cycle_count + session->transition_cycles;
            }
            if (enable_started && !all_axes_enabled &&
                session->report->cycle_count >= enable_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            if (!all_modes_confirmed &&
                session->report->cycle_count >= mode_confirmation_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            if (feedback_valid && !safety_denied &&
                session->plan->motion_profile != NULL && all_axes_enabled &&
                all_modes_confirmed) {
                emaster_relative_motion_status_t motion_status;

                if (!motion_initialized) {
                    if (!session->motion_prepared) {
                        status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                        return status;
                    }
                    motion_initialized = true;
                    session->report->motion_started = true;
                }
                if (session->plan->motion_profile->trajectory ==
                    EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION)
                {
                    motion_status = emaster_relative_motion_step(
                        &session->motion, session->actual_positions, session->target_positions,
                        session->plan->axis_count);
                }
                else if (session->plan->motion_profile->trajectory ==
                         EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY)
                {
                    motion_status = emaster_velocity_motion_step(
                        &session->velocity_motion, session->actual_positions,
                        session->target_positions, session->plan->axis_count);
                }
                else
                {
                    motion_status = EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
                }
                /* 即使本周期因跟随误差退出，也要把触发值保留到会话报告。 */
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    session->axes[axis_index].max_observed_following_error_counts =
                        session->motion_axes[axis_index].max_observed_following_error_counts;
                }
                if (motion_status == EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR) {
                    status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                else if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE &&
                         motion_status != EMASTER_RELATIVE_MOTION_SETTLING &&
                         motion_status != EMASTER_RELATIVE_MOTION_COMPLETE) {
                    status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                if (!safety_denied)
                {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        if (!emaster_soem_axis_set_target_value(
                                &session->plan->axes[axis_index], &session->axes[axis_index],
                                session->target_positions[axis_index]))
                            safety_denied = true;
                    }
                    session->report->motion_completed =
                        motion_status == EMASTER_RELATIVE_MOTION_COMPLETE;
                }
                if (session->report->motion_completed &&
                    session->plan->motion_profile->trajectory ==
                        EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION) {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        emaster_control_session_axis_result_t *axis_result =
                            &session->axes[axis_index];
                        const int32_t planned_angle =
                            session->motion_axis_configs[axis_index]->relative_angle_millidegrees;

                        axis_result->motion_completion_actual_position =
                            axis_result->actual_position;
                        axis_result->motion_actual_delta_counts =
                            (int64_t)axis_result->motion_completion_actual_position -
                            (int64_t)axis_result->initial_actual_position;
                        axis_result->motion_final_error_counts = absolute_position_difference(
                            axis_result->motion_completion_actual_position,
                            axis_result->motion_final_position);
                        axis_result->motion_direction_match =
                            planned_angle == 0
                                ? axis_result->motion_actual_delta_counts == 0
                                : (planned_angle > 0 &&
                                   axis_result->motion_actual_delta_counts > 0) ||
                                      (planned_angle < 0 &&
                                       axis_result->motion_actual_delta_counts < 0);
                        if (!axis_result->motion_direction_match ||
                            axis_result->motion_final_error_counts >
                                axis_result->max_following_error_counts) {
                            status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                            emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                            safety_denied = true;
                        }
                    }
                    if (safety_denied)
                    {
                        session->report->motion_completed = false;
                    }
                }
            }
            {
                safety_status = emaster_soem_session_apply_safety(
                    session, feedback_valid, all_modes_confirmed, safety_status,
                    &safety_denied);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];

                if (!emaster_cia_process_image_update_output(
                        &session->plan->axes[axis_index], &session->images[axis_index],
                        session->controller_outputs[axis_index].control_word,
                        emaster_soem_axis_target_value(&session->plan->axes[axis_index],
                                                       axis_result),
                        session->context.slavelist[axis_index + 1U].outputs,
                        session->context.slavelist[axis_index + 1U].Obytes)) {
                    status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
                    return status;
                }
            }
            if (!safety_denied && all_axes_enabled && all_modes_confirmed)
            {
                emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_RUNNING,
                                               EMASTER_CONTROL_SESSION_OK);
            }
            (void)emaster_soem_session_publish_feedback(session, safety_status);
            if (session->report->motion_completed) {
                break;
            }
            if (safety_denied &&
                (safety_status != EMASTER_CONTROL_SESSION_OK ||
                 session->report->stop_requested))
            {
                emaster_soem_session_set_state(
                    session,
                    safety_status == EMASTER_CONTROL_SESSION_OK
                        ? EMASTER_CONTROL_STATE_STOPPING
                        : EMASTER_CONTROL_STATE_FAULTED,
                    safety_status);
                return emaster_soem_session_publish_feedback(session, safety_status);
            }
        }
    }
    return status;
}
