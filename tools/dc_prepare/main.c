#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/dc_prepare.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 在当前主机上查找唯一匹配的部署配置。
 * 若找到多个匹配，返回 NULL 并输出候选配置列表到 stderr。
 */
static const emaster_deployment_config_t *deployment_for_current_host(void)
{
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    const emaster_deployment_config_t *candidates[16];
    size_t candidate_count = 0U;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0)
    {
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';

    /* 收集所有匹配的配置 */
    for (index = 0U; index < emaster_deployment_config_count(); ++index)
    {
        const emaster_deployment_config_t *candidate =
            emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0)
        {
            if (match == NULL)
            {
                match = candidate;
            }
            if (candidate_count < sizeof(candidates) / sizeof(candidates[0]))
            {
                candidates[candidate_count++] = candidate;
            }
        }
    }

    /* 唯一匹配时返回配置 */
    if (candidate_count == 1U)
    {
        return match;
    }

    /* 多个匹配时输出候选列表并返回 NULL */
    if (candidate_count > 1U)
    {
        fprintf(stderr, "错误：当前主机 %s 有 %zu 个匹配的部署配置：\n",
                hostname, candidate_count);
        for (index = 0U; index < candidate_count; ++index)
        {
            fprintf(stderr, "  %zu. %s",
                    index + 1U, candidates[index]->deployment_id);
            if (candidates[index]->topology != NULL)
            {
                fprintf(stderr, " (拓扑=%s, 从站数=%zu)",
                        candidates[index]->topology->topology_id,
                        candidates[index]->topology->slave_count);
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "请使用 --deployment 参数指定：\n");
        fprintf(stderr, "  %s %s --deployment %s\n",
                program_invocation_short_name,
                emaster_text(EMASTER_TEXT_COMMAND_PREPARE_DC),
                candidates[0]->deployment_id);
    }

    return NULL;
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
    const char *prepare_command = emaster_text(EMASTER_TEXT_COMMAND_PREPARE_DC);

    /* 解析命令行参数：支持 --deployment <id> 指定部署配置 */
    if (argc == 4 && strcmp(argv[1], prepare_command) == 0 &&
        strcmp(argv[2], "--deployment") == 0)
    {
        deployment = emaster_deployment_config_by_id(argv[3]);
        if (deployment == NULL)
        {
            fprintf(stderr, "错误：未找到部署配置 '%s'\n", argv[3]);
            fprintf(stderr, "可用的部署配置：\n");
            for (size_t i = 0U; i < emaster_deployment_config_count(); ++i)
            {
                const emaster_deployment_config_t *cfg = emaster_deployment_config_at(i);
                if (cfg != NULL)
                {
                    fprintf(stderr, "  %s", cfg->deployment_id);
                    if (cfg->hostname != NULL)
                    {
                        fprintf(stderr, " (主机=%s)", cfg->hostname);
                    }
                    fprintf(stderr, "\n");
                }
            }
            return 2;
        }
    }
    else if (argc == 2 && strcmp(argv[1], prepare_command) == 0)
    {
        /* 无额外参数时自动匹配当前主机 */
        deployment = deployment_for_current_host();
        if (deployment == NULL)
        {
            /* deployment_for_current_host 已输出详细错误信息 */
            return 1;
        }
    }
    else
    {
        fprintf(stderr, "用法：%s %s [--deployment <deployment-id>]\n",
                argv[0], prepare_command);
        fprintf(stderr, "  无额外参数：自动匹配当前主机的部署配置\n");
        fprintf(stderr, "  --deployment：显式指定部署配置ID\n");
        return 2;
    }

    if (deployment->topology == NULL)
    {
        fprintf(stderr, "错误：部署配置 '%s' 未指定拓扑\n", deployment->deployment_id);
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
                result_text(axis->sm3_readback_match),
                result_text(axis->mode_value_readback_match),
                result_text(axis->mode_display_readback_match),
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
