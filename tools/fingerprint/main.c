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
 */
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
            /* 同一主机存在多个部署时无法无歧义启动，必须先由配置确定唯一部署。 */
            if (match != NULL)
            {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
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
    deployment = deployment_for_current_host();
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
