#include "console.h"
#include "emaster/messages.h"

#include <stdio.h>

static const char *session_status_text(emaster_control_session_status_t status) {
    switch (status) {
    case EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SDO_WRITE_FAILED);
    case EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SDO_READBACK_FAILED);
    case EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_PROCESS_MAP_FAILED);
    case EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SAFE_OP_NOT_REACHED);
    case EMASTER_CONTROL_SESSION_OUT_OF_MEMORY:
        return emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY);
    case EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_DC_CONFIG_FAILED);
    case EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SYNC0_CONFIG_FAILED);
    case EMASTER_CONTROL_SESSION_WKC_MISMATCH:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_WKC_MISMATCH);
    case EMASTER_CONTROL_SESSION_OP_NOT_REACHED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_OP_NOT_REACHED);
    case EMASTER_CONTROL_SESSION_FEEDBACK_INVALID:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_FEEDBACK_INVALID);
    case EMASTER_CONTROL_SESSION_CONTROLLER_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_CONTROLLER_FAILED);
    case EMASTER_CONTROL_SESSION_DRIVE_FAULT:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_DRIVE_FAULT);
    case EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE);
    case EMASTER_CONTROL_SESSION_MOTION_INVALID:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_MOTION_INVALID);
    case EMASTER_CONTROL_SESSION_FOLLOWING_ERROR:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_FOLLOWING_ERROR);
    case EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_CYCLE_WAIT_FAILED);
    case EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
    case EMASTER_CONTROL_SESSION_DC_SYNC_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_DC_SYNC_FAILED);
    case EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SAFE_STOP_FAILED);
    case EMASTER_CONTROL_SESSION_AUDIT_FAILED:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_AUDIT_FAILED);
    case EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED:
        return emaster_text(EMASTER_TEXT_PROBE_INTERFACE_OPEN_FAILED);
    case EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_INTERFACE_NOT_READY);
    case EMASTER_CONTROL_SESSION_NO_SLAVES:
        return emaster_text(EMASTER_TEXT_PROBE_NO_SLAVES);
    case EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED:
        return emaster_text(EMASTER_TEXT_PROBE_PREOP_NOT_REACHED);
    case EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH:
    case EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH:
    case EMASTER_CONTROL_SESSION_PDO_MISMATCH:
        return emaster_text(EMASTER_TEXT_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH);
    case EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED:
        return emaster_text(EMASTER_TEXT_PROBE_RESTORE_INIT_FAILED);
    case EMASTER_CONTROL_SESSION_INVALID_ARGUMENT:
        return emaster_text(EMASTER_TEXT_PROBE_INVALID_ARGUMENT);
    case EMASTER_CONTROL_SESSION_OK:
        return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SUCCESS);
    }
    return emaster_text(EMASTER_TEXT_PROBE_UNKNOWN);
}

static const char *result_text(bool value) {
    return emaster_text(value ? EMASTER_TEXT_CONTROL_SESSION_MATCH
                              : EMASTER_TEXT_CONTROL_SESSION_MISMATCH);
}

void emaster_master_console_result(const emaster_session_plan_t *plan,
                                   const emaster_control_session_report_t *report, bool published) {
    size_t axis_index;

    for (axis_index = 0U; axis_index < report->axis_count; ++axis_index) {
        const emaster_control_session_axis_result_t *axis = &report->axes[axis_index];
        if (axis->position == 0U) {
            continue;
        }
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_AXIS_LINE),
                (unsigned int)axis->position, result_text(axis->identity_match),
                result_text(axis->pdo_match), result_text(axis->process_map_match),
                result_text(axis->output_initialized), result_text(axis->input_decoded),
                result_text(axis->mode_display_match), (int)axis->requested_mode,
                (int)axis->mode_display, (unsigned int)axis->status_word,
                (unsigned int)axis->control_word, (int)axis->initial_actual_position,
                (int)axis->actual_position, (int)axis->target_position,
                result_text(axis->operation_enabled_seen));
        if (report->safe_op_reached) {
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_SAFEOP_MODE_LINE),
                    (unsigned int)axis->position, result_text(axis->safeop_mode_display_sdo_read),
                    (int)axis->safeop_mode_display_sdo,
                    (unsigned int)axis->safeop_drive_diagnostic.cia402_error_code,
                    (unsigned int)axis->safeop_drive_diagnostic.error_register,
                    (unsigned long)axis->safeop_drive_diagnostic.extended_servo_error_code,
                    (unsigned long)axis->safeop_drive_diagnostic.servo_error_code,
                    result_text(axis->safeop_drive_diagnostic.read_succeeded));
        }
        if (axis->pdo_assignment_failed_index != 0U) {
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_PDO_ASSIGNMENT_LINE),
                    (unsigned int)axis->position, (unsigned int)axis->pdo_assignment_failed_index,
                    (unsigned int)axis->pdo_assignment_failed_subindex,
                    result_text(axis->pdo_assignment_abort_code_available),
                    (unsigned long)axis->pdo_assignment_abort_code);
        }
        if (report->diagnostic_preop_reached) {
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_FINAL_MODE_LINE),
                    (unsigned int)axis->position, result_text(axis->mode_command_sdo_read),
                    (int)axis->mode_command_sdo, result_text(axis->mode_display_sdo_read),
                    (int)axis->mode_display_sdo);
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_DIAGNOSTIC_LINE),
                    (unsigned int)axis->position,
                    result_text(axis->drive_diagnostic.read_succeeded),
                    (unsigned int)axis->drive_diagnostic.cia402_error_code,
                    (unsigned int)axis->drive_diagnostic.error_register,
                    (unsigned long)axis->drive_diagnostic.extended_servo_error_code,
                    (unsigned long)axis->drive_diagnostic.servo_error_code,
                    result_text(axis->sm2_diagnostic.read_succeeded),
                    (unsigned int)axis->sm2_diagnostic.sm_event_missed,
                    (unsigned int)axis->sm2_diagnostic.cycle_time_too_small,
                    (unsigned int)axis->sm2_diagnostic.shift_time_too_short,
                    result_text(axis->sm2_diagnostic.read_succeeded &&
                                !axis->sm2_diagnostic.sync_error),
                    result_text(axis->sm3_diagnostic.read_succeeded),
                    (unsigned int)axis->sm3_diagnostic.sm_event_missed,
                    (unsigned int)axis->sm3_diagnostic.cycle_time_too_small,
                    (unsigned int)axis->sm3_diagnostic.shift_time_too_short,
                    result_text(axis->sm3_diagnostic.read_succeeded &&
                                !axis->sm3_diagnostic.sync_error));
        }
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_SCALE_LINE),
                (unsigned int)axis->position, result_text(axis->position_scale.read_succeeded),
                (unsigned long)axis->position_scale.encoder_increments,
                (unsigned long)axis->position_scale.encoder_motor_revolutions,
                (unsigned long)axis->position_scale.gear_motor_revolutions,
                (unsigned long)axis->position_scale.gear_shaft_revolutions);
        if (report->motion_completed) {
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_MOTION_LINE),
                    (unsigned int)axis->position, result_text(axis->position_scale_match),
                    (int)axis->motion_final_position, (int)axis->motion_completion_actual_position,
                    (long long)axis->motion_actual_delta_counts,
                    (unsigned long long)axis->motion_final_error_counts,
                    result_text(axis->motion_direction_match), (int)axis->target_position,
                    (unsigned long long)axis->max_following_error_counts,
                    (unsigned long long)axis->max_observed_following_error_counts);
        }
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_SUMMARY_LINE),
            result_text(report->safe_op_reached), result_text(report->op_reached),
            result_text(report->all_axes_enabled_reached),
            plan->motion_profile == NULL ? emaster_text(EMASTER_TEXT_CONTROL_SESSION_NOT_CONFIGURED)
                                         : result_text(report->motion_started),
            plan->motion_profile == NULL ? emaster_text(EMASTER_TEXT_CONTROL_SESSION_NOT_CONFIGURED)
                                         : result_text(report->motion_completed),
            (unsigned int)report->expected_wkc, report->actual_wkc,
            (unsigned long long)report->cycle_count, result_text(report->stop_requested),
            result_text(report->safe_output_sent), result_text(report->safe_state_reached),
            result_text(report->sync0_disabled), result_text(report->restore_init_succeeded));
    if (report->first_cycle_failure.present) {
        const emaster_cycle_failure_t *failure = &report->first_cycle_failure;
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_FIRST_FAILURE_LINE),
                (unsigned long long)failure->exchange, session_status_text(failure->status),
                result_text(failure->wkc_available), failure->wkc_available ? failure->wkc : -1);
    }
    if (report->audit.omitted_pdo_samples > 0U) {
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_AUDIT_TRUNCATED),
                (unsigned long long)report->audit.omitted_pdo_samples);
    }
    if (report->status == EMASTER_CONTROL_SESSION_OK) {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SUCCESS), stdout);
    } else {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_FAILED),
                session_status_text(report->status));
    }
    if (published) {
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_REPORT_SAVED),
                plan->deployment->run_report_path);
    } else {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_REPORT_FAILED),
                plan->deployment->run_report_path);
    }
}
