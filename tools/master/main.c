#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/control_session.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static bool application_stop_requested(void *user_data)
{
    (void)user_data;
    return stop_requested != 0;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0)
    {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

static const emaster_deployment_config_t *deployment_for_current_host(void)
{
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0)
    {
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';
    for (index = 0U; index < emaster_deployment_config_count(); ++index)
    {
        const emaster_deployment_config_t *candidate =
            emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0)
        {
            if (match != NULL)
            {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
}

static const char *session_status_text(emaster_control_session_status_t status)
{
    switch (status)
    {
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
        case EMASTER_CONTROL_SESSION_INITIAL_WKC_FAILED:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_INITIAL_WKC_FAILED);
        case EMASTER_CONTROL_SESSION_OP_NOT_REACHED:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_OP_NOT_REACHED);
        case EMASTER_CONTROL_SESSION_FEEDBACK_INVALID:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_FEEDBACK_INVALID);
        case EMASTER_CONTROL_SESSION_CONTROLLER_FAILED:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_CONTROLLER_FAILED);
        case EMASTER_CONTROL_SESSION_DRIVE_FAULT:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_DRIVE_FAULT);
        case EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_CYCLE_WAIT_FAILED);
        case EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED:
            return emaster_text(EMASTER_TEXT_CONTROL_SESSION_SAFE_STOP_FAILED);
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

static const char *result_text(bool value)
{
    return emaster_text(value ? EMASTER_TEXT_CONTROL_SESSION_MATCH
                              : EMASTER_TEXT_CONTROL_SESSION_MISMATCH);
}

int main(int argc, char **argv)
{
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_control_session_axis_result_t *results;
    emaster_control_session_report_t report;
    emaster_session_plan_status_t plan_status;
    emaster_control_session_status_t session_status;
    size_t axis_capacity;
    size_t axis_index;

    if (argc != 1)
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_USAGE), argv[0]);
        return 2;
    }
    if (!install_signal_handlers())
    {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SIGNAL_FAILED), stderr);
        return 1;
    }
    deployment = deployment_for_current_host();
    if (deployment == NULL || deployment->topology == NULL)
    {
        fputs(emaster_text(EMASTER_TEXT_MESSAGE_DEPLOYMENT_UNAVAILABLE), stderr);
        return 1;
    }
    axis_capacity = deployment->topology->slave_count;
    plan_axes = calloc(axis_capacity, sizeof(*plan_axes));
    results = calloc(axis_capacity, sizeof(*results));
    if (plan_axes == NULL || results == NULL)
    {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY), stderr);
        return 1;
    }
    plan_status = emaster_session_plan_build(deployment, plan_axes, axis_capacity, &plan);
    if (plan_status != EMASTER_SESSION_PLAN_READY)
    {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_PLAN_FAILED), stderr);
        return 1;
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_START),
            deployment->hostname, deployment->ethercat_interface,
            deployment->topology->topology_id, (unsigned int)plan.axis_count);
    (void)fflush(stdout);

    session_status = emaster_soem_control_session(
        &plan, results, axis_capacity, application_stop_requested, NULL, &report);
    for (axis_index = 0U; axis_index < report.axis_count; ++axis_index)
    {
        const emaster_control_session_axis_result_t *axis = &results[axis_index];
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_AXIS_LINE),
                (unsigned int)axis->position, result_text(axis->identity_match),
                result_text(axis->pdo_match), result_text(axis->process_map_match),
                result_text(axis->output_initialized), result_text(axis->input_decoded),
                result_text(axis->mode_display_match), (int)axis->requested_mode,
                (int)axis->mode_display, (unsigned int)axis->status_word,
                (unsigned int)axis->control_word, (int)axis->initial_actual_position,
                (int)axis->actual_position, (int)axis->hold_target_position,
                result_text(axis->operation_enabled_seen),
                result_text(axis->mode_command_sdo_read), (int)axis->mode_command_sdo,
                result_text(axis->mode_display_sdo_read), (int)axis->mode_display_sdo,
                result_text(axis->input_mode_sdo_read),
                (unsigned int)axis->input_mode_sdo,
                result_text(axis->switch_on_disabled_seen),
                result_text(axis->ready_to_switch_on_seen),
                result_text(axis->switched_on_seen));
        if (axis->pdo_assignment_failed_index != 0U)
        {
            fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_PDO_ASSIGNMENT_LINE),
                    (unsigned int)axis->position,
                    (unsigned int)axis->pdo_assignment_failed_index,
                    (unsigned int)axis->pdo_assignment_failed_subindex,
                    result_text(axis->pdo_assignment_abort_code_available),
                    (unsigned long)axis->pdo_assignment_abort_code);
        }
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
                result_text(!axis->sm2_diagnostic.sync_error),
                result_text(axis->sm3_diagnostic.read_succeeded),
                (unsigned int)axis->sm3_diagnostic.sm_event_missed,
                (unsigned int)axis->sm3_diagnostic.cycle_time_too_small,
                (unsigned int)axis->sm3_diagnostic.shift_time_too_short,
                result_text(!axis->sm3_diagnostic.sync_error));
        fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_SCALE_LINE),
                (unsigned int)axis->position,
                result_text(axis->position_scale.read_succeeded),
                (unsigned long)axis->position_scale.encoder_increments,
                (unsigned long)axis->position_scale.encoder_motor_revolutions,
                (unsigned long)axis->position_scale.gear_motor_revolutions,
                (unsigned long)axis->position_scale.gear_shaft_revolutions);
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_SUMMARY_LINE),
            result_text(report.safe_op_reached), result_text(report.op_reached),
            result_text(report.all_axes_enabled_reached),
            (unsigned int)report.expected_wkc, report.actual_wkc,
            (unsigned long long)report.cycle_count, result_text(report.stop_requested),
            result_text(report.safe_output_sent), result_text(report.safe_state_reached),
            result_text(report.sync0_disabled),
            result_text(report.restore_init_succeeded));
    if (session_status == EMASTER_CONTROL_SESSION_OK)
    {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SUCCESS), stdout);
    }
    else
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_FAILED),
                session_status_text(session_status));
    }
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK ? 0 : 1;
}
