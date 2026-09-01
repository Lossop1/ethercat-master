#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/dc_prepare.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool confirm_prepare(const emaster_deployment_config_t *deployment,
                            size_t axis_count)
{
    char input[32];
    size_t length;

    if (deployment == NULL || !isatty(STDIN_FILENO))
    {
        fputs(emaster_text(EMASTER_TEXT_DC_PREPARE_CONFIRM_REQUIRED), stderr);
        return false;
    }
    fprintf(stderr, emaster_text(EMASTER_TEXT_DC_PREPARE_CONFIRM_PROMPT),
            deployment->hostname, deployment->ethercat_interface,
            deployment->topology->topology_id, axis_count,
            emaster_text(EMASTER_TEXT_DC_PREPARE_CONFIRMATION_TOKEN));
    if (fflush(stderr) != 0 || fgets(input, sizeof(input), stdin) == NULL)
    {
        return false;
    }
    length = strcspn(input, "\r\n");
    input[length] = '\0';
    return strcmp(input, emaster_text(EMASTER_TEXT_DC_PREPARE_CONFIRMATION_TOKEN)) == 0;
}

static const char *prepare_status_text(emaster_dc_prepare_status_t status)
{
    switch (status)
    {
        case EMASTER_DC_PREPARE_DC_UNAVAILABLE:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_DC_UNAVAILABLE);
        case EMASTER_DC_PREPARE_SDO_WRITE_FAILED:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_SDO_WRITE_FAILED);
        case EMASTER_DC_PREPARE_SDO_READBACK_FAILED:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_SDO_READBACK_FAILED);
        case EMASTER_DC_PREPARE_DC_CONFIG_FAILED:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_DC_CONFIG_FAILED);
        case EMASTER_DC_PREPARE_SYNC0_READBACK_FAILED:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_SYNC0_READBACK_FAILED);
        case EMASTER_DC_PREPARE_INTERFACE_OPEN_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_INTERFACE_OPEN_FAILED);
        case EMASTER_DC_PREPARE_NO_SLAVES:
            return emaster_text(EMASTER_TEXT_PROBE_NO_SLAVES);
        case EMASTER_DC_PREPARE_PREOP_NOT_REACHED:
            return emaster_text(EMASTER_TEXT_PROBE_PREOP_NOT_REACHED);
        case EMASTER_DC_PREPARE_TOPOLOGY_MISMATCH:
        case EMASTER_DC_PREPARE_IDENTITY_MISMATCH:
        case EMASTER_DC_PREPARE_PDO_MISMATCH:
            return emaster_text(EMASTER_TEXT_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH);
        case EMASTER_DC_PREPARE_RESTORE_INIT_FAILED:
            return emaster_text(EMASTER_TEXT_PROBE_RESTORE_INIT_FAILED);
        case EMASTER_DC_PREPARE_INVALID_ARGUMENT:
            return emaster_text(EMASTER_TEXT_PROBE_INVALID_ARGUMENT);
        case EMASTER_DC_PREPARE_OK:
            return emaster_text(EMASTER_TEXT_DC_PREPARE_SUCCESS);
    }
    return emaster_text(EMASTER_TEXT_PROBE_UNKNOWN);
}

static const char *result_text(bool value)
{
    return emaster_text(value ? EMASTER_TEXT_DC_PREPARE_MATCH
                              : EMASTER_TEXT_DC_PREPARE_MISMATCH);
}

int main(int argc, char **argv)
{
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_dc_prepare_axis_result_t *results;
    emaster_dc_prepare_report_t report;
    emaster_session_plan_status_t plan_status;
    emaster_dc_prepare_status_t prepare_status;
    size_t axis_capacity;
    size_t axis_index;

    if (argc != 2 || strcmp(argv[1], emaster_text(EMASTER_TEXT_COMMAND_PREPARE_DC)) != 0)
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_DC_PREPARE_USAGE), argv[0],
                emaster_text(EMASTER_TEXT_COMMAND_PREPARE_DC));
        return 2;
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
        fputs(emaster_text(EMASTER_TEXT_DC_PREPARE_PLAN_FAILED), stderr);
        return 1;
    }
    if (!confirm_prepare(deployment, plan.axis_count))
    {
        free(plan_axes);
        free(results);
        return 1;
    }

    prepare_status = emaster_soem_dc_prepare(&plan, results, axis_capacity, &report);
    for (axis_index = 0U; axis_index < report.axis_count; ++axis_index)
    {
        const emaster_dc_prepare_axis_result_t *axis = &results[axis_index];
        fprintf(stdout, emaster_text(EMASTER_TEXT_DC_PREPARE_AXIS_LINE),
                (unsigned int)axis->position, result_text(axis->identity_match),
                result_text(axis->pdo_match), result_text(axis->sm2_readback_match),
                result_text(axis->sm3_readback_match), result_text(axis->mode_readback_match),
                result_text(axis->sync0_readback_match));
    }
    if (prepare_status == EMASTER_DC_PREPARE_OK)
    {
        fputs(emaster_text(EMASTER_TEXT_DC_PREPARE_SUCCESS), stdout);
    }
    else
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_DC_PREPARE_FAILED),
                prepare_status_text(prepare_status));
    }
    free(plan_axes);
    free(results);
    return prepare_status == EMASTER_DC_PREPARE_OK ? 0 : 1;
}
