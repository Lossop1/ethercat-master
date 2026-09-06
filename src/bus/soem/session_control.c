#include "session_internal.h"

#include <limits.h>
#include <string.h>

static uint64_t absolute_position_difference(int32_t left, int32_t right) {
    int64_t difference = (int64_t)left - (int64_t)right;

    return (uint64_t)(difference < 0 ? -difference : difference);
}

emaster_control_session_status_t emaster_soem_session_run(emaster_soem_session_t *session) {
    size_t axis_index;
    bool motion_initialized = false;
    bool mode_confirmation_started = false;
    uint64_t mode_confirmation_deadline_cycle = 0U;
    uint64_t enable_deadline_cycle;
    emaster_control_session_status_t status = EMASTER_CONTROL_SESSION_OK;

    enable_deadline_cycle = session->report->cycle_count + session->transition_cycles;
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
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        if (!emaster_cia402_controller_set_goal(&session->controllers[axis_index],
                                                EMASTER_CIA402_GOAL_OPERATION_ENABLED)) {
            status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
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

            status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return status;
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
                    status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                    return status;
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
                session->axes[axis_index].actual_position = actual_position;
                session->status_words[axis_index] = status_word;
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
                return status;
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
                    return status;
                }
                if (!operation_enabled) {
                    all_axes_enabled = false;
                }
            }
            session->report->all_axes_enabled_reached |= all_axes_enabled;
            if (!all_axes_enabled && session->report->cycle_count >= enable_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                return status;
            }
            if (all_axes_enabled && !mode_confirmation_started) {
                mode_confirmation_started = true;
                mode_confirmation_deadline_cycle =
                    session->report->cycle_count + session->transition_cycles;
            }
            if (mode_confirmation_started && !all_modes_confirmed &&
                session->report->cycle_count >= mode_confirmation_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                return status;
            }
            if (session->plan->motion_profile != NULL && all_axes_enabled && all_modes_confirmed) {
                emaster_relative_motion_status_t motion_status;

                if (!motion_initialized) {
                    int32_t *initial_positions = session->target_positions;

                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        initial_positions[axis_index] =
                            session->axes[axis_index].initial_actual_position;
                    }
                    motion_status = emaster_relative_motion_init(
                        session->plan->motion_profile, session->motion_axis_configs,
                        session->motion_scales, initial_positions, session->plan->axis_count,
                        session->plan->cycle_ns, session->motion_axes, &session->motion);
                    if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE) {
                        status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                        return status;
                    }
                    motion_initialized = true;
                    session->report->motion_started = true;
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        session->axes[axis_index].motion_final_position =
                            session->motion_axes[axis_index].final_position;
                        session->axes[axis_index].max_following_error_counts =
                            session->motion_axes[axis_index].max_following_error_counts;
                    }
                }
                motion_status = emaster_relative_motion_step(
                    &session->motion, session->actual_positions, session->target_positions,
                    session->plan->axis_count);
                /* 即使本周期因跟随误差退出，也要把触发值保留到会话报告。 */
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    session->axes[axis_index].max_observed_following_error_counts =
                        session->motion_axes[axis_index].max_observed_following_error_counts;
                }
                if (motion_status == EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR) {
                    status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                    return status;
                }
                if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE &&
                    motion_status != EMASTER_RELATIVE_MOTION_SETTLING &&
                    motion_status != EMASTER_RELATIVE_MOTION_COMPLETE) {
                    status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                    return status;
                }
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    session->axes[axis_index].target_position =
                        session->target_positions[axis_index];
                }
                session->report->motion_completed =
                    motion_status == EMASTER_RELATIVE_MOTION_COMPLETE;
                if (session->report->motion_completed) {
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
                            return status;
                        }
                    }
                }
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];

                if (!emaster_cia_process_image_update_output(
                        &session->plan->axes[axis_index], &session->images[axis_index],
                        session->controller_outputs[axis_index].control_word,
                        axis_result->target_position,
                        session->context.slavelist[axis_index + 1U].outputs,
                        session->context.slavelist[axis_index + 1U].Obytes)) {
                    status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
                    return status;
                }
            }
            if (session->report->motion_completed) {
                break;
            }
            if (session->stop_requested != NULL &&
                session->stop_requested(session->stop_user_data)) {
                session->report->stop_requested = true;
                break;
            }
        }
    }
    return status;
}
