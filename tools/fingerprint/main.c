#define _POSIX_C_SOURCE 200809L

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

static void usage(const char *program)
{
    fprintf(stderr,
            "用法：\n"
            "  %s --list-interfaces\n"
            "  %s --list-deployments\n"
            "  %s --deployment DEPLOYMENT_ID --output FILE --acknowledge-preop\n\n"
            "探测会发送 EtherCAT 发现报文并请求 PRE-OP。它只读取 SDO，不映射 PDO、不配置 "
            "DC，也不请求 SAFE-OP 或 OP；关闭前会尝试恢复 INIT。\n",
            program, program, program);
}

static void print_interface(const char *name, const char *description, void *user_data)
{
    FILE *output = user_data;
    fprintf(output, "%s\t%s\n", name, description != NULL ? description : "");
}

static void print_deployment(const emaster_deployment_config_t *deployment)
{
    printf("%s\t主机=%s\t接口=%s\t拓扑=%s\n", deployment->deployment_id,
           deployment->hostname, deployment->ethercat_interface,
           deployment->topology->topology_id);
}

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
static bool host_matches_deployment(const emaster_deployment_config_t *deployment)
{
    char hostname[256];

    if (deployment == NULL || deployment->hostname == NULL || deployment->hostname[0] == '\0' ||
        gethostname(hostname, sizeof(hostname) - 1U) != 0)
    {
        return false;
    }
    hostname[sizeof(hostname) - 1U] = '\0';
    return strcmp(hostname, deployment->hostname) == 0;
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
            !emaster_slave_identity_matches(profile, &actual->identity))
        {
            return false;
        }
    }
    return true;
}

static void utc_timestamp(char *buffer, size_t capacity)
{
    time_t now = time(NULL);
    struct tm result;

    if (gmtime_r(&now, &result) == NULL ||
        strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &result) == 0U)
    {
        (void)snprintf(buffer, capacity, "unknown");
    }
}

static const char *probe_error(emaster_preop_probe_status_t status)
{
    switch (status)
    {
        case EMASTER_PREOP_PROBE_INVALID_ARGUMENT:
            return "PRE-OP 探测参数无效";
        case EMASTER_PREOP_PROBE_INTERFACE_ENUMERATION_FAILED:
            return "没有找到网络接口";
        case EMASTER_PREOP_PROBE_INTERFACE_OPEN_FAILED:
            return "无法打开 EtherCAT 原始套接字";
        case EMASTER_PREOP_PROBE_NO_SLAVES:
            return "没有发现 EtherCAT 从站";
        case EMASTER_PREOP_PROBE_TOO_MANY_SLAVES:
            return "发现的 EtherCAT 从站数量超过指纹格式上限";
        case EMASTER_PREOP_PROBE_PREOP_NOT_REACHED:
            return "一个或多个从站未确认进入 PRE-OP";
        case EMASTER_PREOP_PROBE_SDO_READ_FAILED:
            return "一个或多个 CoE 从站的 SDO 读取全部失败";
        case EMASTER_PREOP_PROBE_OUT_OF_MEMORY:
            return "无法为指纹报告分配内存";
        case EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED:
            return "一个或多个从站未确认恢复 INIT";
        case EMASTER_PREOP_PROBE_OK:
            return "成功";
    }
    return "未知 PRE-OP 探测错误";
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
        fprintf(stderr, "输出路径过长。\n");
        return 1;
    }

    /*
     * 先独占创建同目录临时文件，完整关闭后再用硬链接发布。这样既拒绝覆盖已有证据，
     * 也不会让崩溃或写满磁盘留下一个看似完整的目标文件。
     */
    file_descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (file_descriptor < 0)
    {
        fprintf(stderr, "无法创建 %s：%s\n", temporary_path, strerror(errno));
        return 1;
    }
    output = fdopen(file_descriptor, "w");
    if (output == NULL)
    {
        fprintf(stderr, "无法打开指纹输出流：%s\n", strerror(errno));
        close(file_descriptor);
        unlink(temporary_path);
        return 1;
    }

    if (emaster_fingerprint_write_json(output, report, deployment, timestamp) != 0)
    {
        fprintf(stderr, "写入指纹失败。\n");
        (void)fclose(output);
        unlink(temporary_path);
    }
    else if (fclose(output) != 0)
    {
        fprintf(stderr, "刷新指纹文件失败：%s\n", strerror(errno));
        unlink(temporary_path);
    }
    else if (link(temporary_path, output_path) != 0)
    {
        fprintf(stderr, "发布指纹文件失败：%s\n", strerror(errno));
        unlink(temporary_path);
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
    const char *deployment_id = NULL;
    const emaster_deployment_config_t *deployment;
    const char *output_path = NULL;
    emaster_sdo_request_t requests[EMASTER_PREOP_MAX_SDO_REQUESTS];
    emaster_preop_report_t report = {0};
    emaster_preop_probe_status_t status;
    size_t request_count;
    char timestamp[32];
    int acknowledge_preop = 0;
    int report_is_usable;
    int index;
    int output_result;

    if (argc == 2 && strcmp(argv[1], "--list-interfaces") == 0)
    {
        status = emaster_soem_visit_interfaces(print_interface, stdout);
        if (status != EMASTER_PREOP_PROBE_OK)
        {
            fprintf(stderr, "%s。\n", probe_error(status));
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--list-deployments") == 0)
    {
        size_t deployment_index;
        for (deployment_index = 0U;
             deployment_index < emaster_deployment_config_count(); ++deployment_index)
        {
            print_deployment(emaster_deployment_config_at(deployment_index));
        }
        return 0;
    }

    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--deployment") == 0 && index + 1 < argc)
        {
            deployment_id = argv[++index];
        }
        else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc)
        {
            output_path = argv[++index];
        }
        else if (strcmp(argv[index], "--acknowledge-preop") == 0)
        {
            acknowledge_preop = 1;
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    if (deployment_id == NULL || output_path == NULL || !acknowledge_preop)
    {
        usage(argv[0]);
        return 2;
    }
    deployment = emaster_deployment_config_by_id(deployment_id);
    if (!deployment_is_eligible(deployment))
    {
        fprintf(stderr, "部署不存在或内容不完整：%s\n", deployment_id);
        return 1;
    }
    if (!host_matches_deployment(deployment))
    {
        fprintf(stderr, "当前主机与部署配置不一致，拒绝打开 EtherCAT 接口。\n");
        return 1;
    }
    if (access(output_path, F_OK) == 0)
    {
        fprintf(stderr, "拒绝覆盖已有指纹文件：%s\n", output_path);
        return 1;
    }

    if (!emaster_fingerprint_sdo_plan(requests, sizeof(requests) / sizeof(requests[0]),
                                      &request_count))
    {
        fprintf(stderr, "无法根据设备目录生成 SDO 读取计划。\n");
        return 1;
    }
    utc_timestamp(timestamp, sizeof(timestamp));
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
                       status == EMASTER_PREOP_PROBE_SDO_READ_FAILED;
    if (!report_is_usable)
    {
        fprintf(stderr, "%s，接口：%s。\n", probe_error(status),
                deployment->ethercat_interface);
        if (report.slave_count > 0U && !report.restore_init_succeeded)
        {
            fprintf(stderr, "警告：%s。\n",
                    probe_error(EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED));
            emaster_preop_report_destroy(&report);
            return 2;
        }
        emaster_preop_report_destroy(&report);
        return 1;
    }

    if (report.slave_count == 0U)
    {
        fprintf(stderr, "警告：%s。\n", probe_error(status));
        emaster_preop_report_destroy(&report);
        return 2;
    }

    if (!report_matches_topology(&report, deployment))
    {
        fprintf(stderr, "物理从站报告与部署拓扑不一致，拒绝发布指纹。\n");
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
        fprintf(stderr, "警告：%s。\n", probe_error(status));
        return 2;
    }
    if (status == EMASTER_PREOP_PROBE_SDO_READ_FAILED)
    {
        fprintf(stderr, "错误：%s。已保存诊断报告，但该报告不能作为通过证据。\n",
                probe_error(status));
        return 1;
    }
    return 0;
}
