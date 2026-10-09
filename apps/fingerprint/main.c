#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "fingerprint_json.h"
#include "fingerprint_plan.h"

#include "emaster/bus/preop_probe.h"
#include "emaster/catalog/slave_profile.h"
#include "emaster/config/runtime_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * 在发送任何 EtherCAT 帧前确认部署和拓扑已经由配置明确选择。status 只作为用户配置事实
 * 保存，基础设施不擅自把某个字符串解释为审批授权。
 */
static bool deployment_is_eligible(const emaster_deployment_config_t *deployment)
{
    return deployment != NULL && deployment->topology != NULL &&
           deployment->ethercat_interface != NULL && deployment->ethercat_interface[0] != '\0';
}

/*
 * 部署配置还声明目标主机。启动时做精确比较，防止把某台主机的网卡参数误用于另一台主机；
 * 获取不到主机名或配置过长都按失败处理，不使用猜测值。
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
        fprintf(stderr, "  emaster-fingerprint --deployment %s\n",
                candidates[0]->deployment_id);
    }

    return NULL;
}

/*
 * 把物理报告与选定拓扑逐项比较。这里不假设轴数；期望数量和 profile_id 均来自部署引用的
 * 拓扑配置。任一位置缺失、额外、身份不符或目录不存在，都必须阻断而不能降级为告警。
 */
static bool report_matches_topology(const emaster_preop_report_t *report,
                                    const emaster_deployment_config_t *deployment)
{
    const emaster_topology_config_t *topology;
    size_t index;

    if (report == NULL || deployment == NULL || deployment->ethercat_interface == NULL ||
        deployment->topology == NULL || deployment->topology->slaves == NULL)
    {
        return false;
    }
    topology = deployment->topology;
    if (strcmp(report->interface_name, deployment->ethercat_interface) != 0 ||
        report->slave_count != topology->slave_count || report->slaves == NULL)
    {
        return false;
    }
    for (index = 0U; index < topology->slave_count; ++index)
    {
        const emaster_topology_slave_config_t *expected = &topology->slaves[index];
        const emaster_preop_slave_t *actual = &report->slaves[index];
        const emaster_slave_profile_t *profile =
            emaster_slave_profile_by_id(expected->profile_id);
        if (profile == NULL || actual->position != expected->position ||
            !emaster_slave_identity_matches(profile, &actual->identity) ||
            !emaster_slave_pdo_layout_matches(profile, &actual->pdo_layout))
        {
            return false;
        }
    }
    return true;
}

/*
 * 拓扑不匹配时必须指出具体字段，否则现场只能看到一个笼统的拒绝。
 * 这里只打印内存中的期望值与实测值，不访问总线。
 */
static void print_topology_mismatch(const emaster_preop_report_t *report,
                                    const emaster_deployment_config_t *deployment)
{
    const emaster_topology_config_t *topology = deployment->topology;
    size_t index;

    if (topology == NULL || topology->slaves == NULL)
    {
        fprintf(stderr, "  部署未引用有效拓扑。\n");
        return;
    }
    if (strcmp(report->interface_name, deployment->ethercat_interface) != 0)
    {
        fprintf(stderr, "  接口不一致：部署=%s 实测=%s\n", deployment->ethercat_interface,
                report->interface_name);
    }
    if (report->slave_count != topology->slave_count)
    {
        fprintf(stderr, "  从站数量不一致：部署=%zu 实测=%zu\n", topology->slave_count,
                report->slave_count);
    }
    for (index = 0U; index < report->slave_count; ++index)
    {
        const emaster_preop_slave_t *actual = &report->slaves[index];

        if (index >= topology->slave_count)
        {
            fprintf(stderr,
                    "  多余从站：位置 %u 名称=%s vendor=0x%08X product=0x%08X "
                    "revision=0x%08X rx位长=%u tx位长=%u\n",
                    (unsigned int)actual->position, actual->name,
                    (unsigned int)actual->identity.vendor_id,
                    (unsigned int)actual->identity.product_code,
                    (unsigned int)actual->identity.revision,
                    (unsigned int)actual->pdo_layout.rx.bit_length,
                    (unsigned int)actual->pdo_layout.tx.bit_length);
        }
    }
    for (index = 0U; index < topology->slave_count && index < report->slave_count; ++index)
    {
        const emaster_topology_slave_config_t *expected = &topology->slaves[index];
        const emaster_preop_slave_t *actual = &report->slaves[index];
        const emaster_slave_profile_t *profile =
            emaster_slave_profile_by_id(expected->profile_id);

        if (profile == NULL)
        {
            fprintf(stderr, "  位置 %u：部署引用的 profile_id %s 不在设备目录中\n",
                    (unsigned int)expected->position, expected->profile_id);
            continue;
        }
        if (actual->position != expected->position)
        {
            fprintf(stderr, "  位置不一致：部署=%u 实测=%u\n",
                    (unsigned int)expected->position, (unsigned int)actual->position);
        }
        if (!emaster_slave_identity_matches(profile, &actual->identity))
        {
            fprintf(stderr,
                    "  位置 %u 身份不一致（%s）：部署 vendor=0x%08X product=0x%08X "
                    "revision=0x%08X，实测 vendor=0x%08X product=0x%08X revision=0x%08X\n",
                    (unsigned int)expected->position, profile->profile_id,
                    (unsigned int)profile->identity.vendor_id,
                    (unsigned int)profile->identity.product_code,
                    (unsigned int)profile->identity.revision,
                    (unsigned int)actual->identity.vendor_id,
                    (unsigned int)actual->identity.product_code,
                    (unsigned int)actual->identity.revision);
        }
        if (!emaster_slave_pdo_layout_matches(profile, &actual->pdo_layout))
        {
            const emaster_pdo_set_profile_t *expected_set =
                emaster_slave_reference_pdo_set(profile);
            fprintf(stderr,
                    "  位置 %u PDO 不一致（%s）：状态=%u 失败索引=0x%04X:%u "
                    "rx位长=%u tx位长=%u",
                    (unsigned int)expected->position, profile->profile_id,
                    (unsigned int)actual->pdo_layout.status,
                    (unsigned int)actual->pdo_layout.failed_index,
                    (unsigned int)actual->pdo_layout.failed_subindex,
                    (unsigned int)actual->pdo_layout.rx.bit_length,
                    (unsigned int)actual->pdo_layout.tx.bit_length);
            if (expected_set != NULL)
            {
                fprintf(stderr, "，期望 rx字节=%u tx字节=%u 方案=%s",
                        (unsigned int)expected_set->rx_pdo_bytes,
                        (unsigned int)expected_set->tx_pdo_bytes, expected_set->pdo_set_id);
            }
            fprintf(stderr, "\n");
        }
    }
}

static bool utc_timestamp(char *buffer, size_t capacity)
{
    time_t now = time(NULL);
    struct tm result;

    return gmtime_r(&now, &result) != NULL &&
           strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &result) != 0U;
}

static int publish_report(const char *output_path, const emaster_preop_report_t *report,
                          const emaster_deployment_config_t *deployment,
                          const char *timestamp)
{
    char temporary_path[4096];
    int file_descriptor;
    FILE *output;
    int result = 1;

    if (snprintf(temporary_path, sizeof(temporary_path), "%s.partial.%ld", output_path,
                 (long)getpid()) >= (int)sizeof(temporary_path))
    {
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_PATH_TOO_LONG, NULL, 0);
        return 1;
    }

    /*
     * 先独占创建同目录临时文件，完整关闭后再用硬链接发布。这样既拒绝覆盖已有证据，
     * 也不会让崩溃或写满磁盘留下一个看似完整的目标文件。
     */
    file_descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (file_descriptor < 0)
    {
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_CREATE_FAILED, NULL, errno);
        return 1;
    }
    output = fdopen(file_descriptor, "w");
    if (output == NULL)
    {
        int system_error = errno;
        close(file_descriptor);
        unlink(temporary_path);
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_OPEN_FAILED, NULL, system_error);
        return 1;
    }

    if (emaster_fingerprint_write_json(output, report, deployment, timestamp) != 0)
    {
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_WRITE_FAILED, NULL, 0);
        (void)fclose(output);
        unlink(temporary_path);
    }
    else if (fclose(output) != 0)
    {
        int system_error = errno;
        unlink(temporary_path);
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_FLUSH_FAILED, NULL, system_error);
    }
    else if (link(temporary_path, output_path) != 0)
    {
        int system_error = errno;
        unlink(temporary_path);
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_PUBLISH_FAILED, NULL,
                                      system_error);
    }
    else
    {
        unlink(temporary_path);
        result = 0;
    }
    return result;
}

int main(int argc, char **argv)
{
    emaster_command_t command = emaster_console_parse_command(argc, argv);
    const emaster_deployment_config_t *deployment;
    const char *output_path;
    emaster_sdo_request_t requests[EMASTER_PREOP_MAX_SDO_REQUESTS];
    emaster_preop_report_t report = {0};
    emaster_preop_probe_status_t status;
    size_t request_count;
    char timestamp[32];
    int report_is_usable;
    int output_result;

    if (command.kind == EMASTER_COMMAND_INTERFACES)
    {
        status = emaster_soem_visit_interfaces(emaster_console_print_interface, stdout);
        if (status != EMASTER_PREOP_PROBE_OK)
        {
            emaster_console_print_probe_status(status, NULL, false);
            return 1;
        }
        return 0;
    }
    if (command.kind == EMASTER_COMMAND_DEPLOYMENTS)
    {
        size_t deployment_index;
        for (deployment_index = 0U;
             deployment_index < emaster_deployment_config_count(); ++deployment_index)
        {
            emaster_console_print_deployment(emaster_deployment_config_at(deployment_index));
        }
        return 0;
    }

    if (command.kind != EMASTER_COMMAND_CAPTURE)
    {
        emaster_console_print_usage(argv[0]);
        return 2;
    }
    output_path = command.output_path;

    /* 解析部署配置：支持 --deployment 显式指定或自动匹配 */
    if (command.deployment_id != NULL)
    {
        deployment = emaster_deployment_config_by_id(command.deployment_id);
        if (deployment == NULL)
        {
            fprintf(stderr, "错误：未找到部署配置 '%s'\n", command.deployment_id);
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
    else
    {
        /* 无 --deployment 参数时自动匹配当前主机 */
        deployment = deployment_for_current_host();
        if (deployment == NULL)
        {
            /* deployment_for_current_host 已输出详细错误信息 */
            return 1;
        }
    }

    if (!deployment_is_eligible(deployment))
    {
        emaster_console_print_message(EMASTER_MESSAGE_DEPLOYMENT_UNAVAILABLE, NULL, 0);
        return 1;
    }
    if (access(output_path, F_OK) == 0)
    {
        emaster_console_print_message(EMASTER_MESSAGE_OUTPUT_EXISTS, output_path, 0);
        return 1;
    }

    if (!emaster_fingerprint_sdo_plan(requests, sizeof(requests) / sizeof(requests[0]),
                                      &request_count))
    {
        emaster_console_print_message(EMASTER_MESSAGE_SDO_PLAN_FAILED, NULL, 0);
        return 1;
    }
    if (!emaster_console_confirm_preop(deployment))
    {
        emaster_console_print_message(EMASTER_MESSAGE_PREOP_NOT_CONFIRMED, NULL, 0);
        return 1;
    }
    if (!utc_timestamp(timestamp, sizeof(timestamp)))
    {
        emaster_console_print_message(EMASTER_MESSAGE_TIMESTAMP_FAILED, NULL, 0);
        return 1;
    }
    status = emaster_soem_preop_probe(deployment->ethercat_interface, requests, request_count,
                                       &report);
    if ((status == EMASTER_PREOP_PROBE_OK ||
         status == EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED) &&
        !emaster_fingerprint_has_sdo_evidence(&report))
    {
        status = EMASTER_PREOP_PROBE_SDO_READ_FAILED;
    }
    report_is_usable = status == EMASTER_PREOP_PROBE_OK ||
                       status == EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED ||
                       status == EMASTER_PREOP_PROBE_SDO_READ_FAILED ||
                       status == EMASTER_PREOP_PROBE_PDO_DISCOVERY_FAILED;
    if (!report_is_usable)
    {
        emaster_console_print_probe_status(status, deployment->ethercat_interface, false);
        if (report.slave_count > 0U && !report.restore_init_succeeded)
        {
            emaster_console_print_probe_status(EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED,
                                               NULL, true);
            emaster_preop_report_destroy(&report);
            return 2;
        }
        emaster_preop_report_destroy(&report);
        return 1;
    }

    if (report.slave_count == 0U)
    {
        emaster_console_print_message(EMASTER_MESSAGE_EMPTY_REPORT, NULL, 0);
        emaster_preop_report_destroy(&report);
        return 2;
    }

    if (!report_matches_topology(&report, deployment))
    {
        size_t slave_index;
        emaster_console_print_message(EMASTER_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH, NULL, 0);
        for (slave_index = 0U; slave_index < report.slave_count; ++slave_index)
        {
            emaster_console_print_pdo_failure(&report.slaves[slave_index].pdo_layout,
                                              report.slaves[slave_index].position);
        }
        print_topology_mismatch(&report, deployment);
        emaster_preop_report_destroy(&report);
        return 1;
    }

    output_result = publish_report(output_path, &report, deployment, timestamp);
    emaster_preop_report_destroy(&report);
    if (output_result != 0)
    {
        return output_result;
    }
    if (status == EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED)
    {
        emaster_console_print_probe_status(status, NULL, true);
        return 2;
    }
    if (status == EMASTER_PREOP_PROBE_SDO_READ_FAILED)
    {
        emaster_console_print_probe_status(status, NULL, false);
        emaster_console_print_message(EMASTER_MESSAGE_DIAGNOSTIC_REPORT_SAVED, NULL, 0);
        return 1;
    }
    if (status == EMASTER_PREOP_PROBE_PDO_DISCOVERY_FAILED)
    {
        emaster_console_print_probe_status(status, NULL, false);
        emaster_console_print_message(EMASTER_MESSAGE_INCOMPLETE_REPORT_REJECTED, NULL, 0);
        return 1;
    }
    return 0;
}
