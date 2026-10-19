#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/dc_prepare.h"
#include "emaster/cli.h"
#include "emaster/cli/deployment_for_host.h"
#include "emaster/cli/option_match.h"
#include "emaster/config/runtime_config.h"
#include "emaster/config/runtime_config_cli.h"
#include "emaster/config/runtime_config_load.h"
#include "emaster/messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 在当前主机上查找唯一匹配的部署配置。
 * 若找到多个匹配，返回 NULL 并输出候选配置列表到 stderr。
 */
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

static void print_usage(FILE *out, const char *program, const char *command)
{
    fprintf(out, "用法：%s %s [--config-dir <目录>] [--deployment <部署ID>]\n",
            program, command);
    fprintf(out, "  无额外参数：自动匹配当前主机的部署配置\n");
    fprintf(out, "  --config-dir：运行时配置目录，默认 config-runtime\n");
    fprintf(out, "  --deployment：显式指定部署配置ID\n");
    fprintf(out, "  --help：打印本说明后退出\n");
    fprintf(out, "  --version：打印版本号后退出\n");
}

int main(int argc, char **argv)
{
    const char *config_directory = emaster_cli_config_dir(&argc, argv);
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

    /* 没有子命令就做不了任何事，所以无参数出帮助并退出 0。 */
    if (argc < 2)
    {
        print_usage(stdout, argv[0], prepare_command);
        return 0;
    }
    switch (emaster_cli_info(argc, argv))
    {
    case EMASTER_CLI_HELP:
        print_usage(stdout, argv[0], prepare_command);
        return 0;
    case EMASTER_CLI_VERSION:
        emaster_cli_print_version("emaster-dc-prepare");
        return 0;
    default:
        break;
    }

    if (config_directory == NULL)
    {
        print_usage(stderr, argv[0], prepare_command);
        return 2;
    }
    if (!emaster_runtime_config_load(config_directory))
    {
        fprintf(stderr, "错误：无法从 '%s' 加载运行时配置\n", config_directory);
        return 1;
    }

    /* 解析命令行参数：支持 --deployment <id> 与 --deployment=<id> 指定部署配置 */
    if (argc >= 2 && strcmp(argv[1], prepare_command) == 0)
    {
        if (argc == 2)
        {
            /* 无额外参数时自动匹配当前主机 */
            deployment = emaster_cli_deployment_for_host("emaster-dc-prepare", prepare_command);
            if (deployment == NULL)
            {
                /* 共享实现已输出原因与下一步 */
                return 1;
            }
        }
        else
        {
            const char *value = NULL;
            bool consumed_next = false;
            const emaster_cli_option_result_t result = emaster_cli_option_match(
                argv[2], "--deployment", (argc > 3) ? argv[3] : NULL, &value, &consumed_next);
            const int consumed = consumed_next ? 3 : 2;

            if (result != EMASTER_CLI_OPTION_VALUE || consumed != argc - 1)
            {
                print_usage(stderr, argv[0], prepare_command);
                return 2;
            }
            deployment = emaster_deployment_config_by_id(value);
            if (deployment == NULL)
            {
                fprintf(stderr, "错误：未找到部署配置 '%s'\n", value);
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
    }
    else
    {
        print_usage(stderr, argv[0], prepare_command);
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
        emaster_session_plan_report_failure(
            plan_status, &plan, emaster_text(EMASTER_TEXT_DC_PREPARE_PLAN_FAILED));
        free(plan_axes);
        free(results);
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
