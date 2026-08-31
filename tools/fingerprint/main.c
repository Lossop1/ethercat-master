#define _POSIX_C_SOURCE 200809L

#include "fingerprint_json.h"
#include "fingerprint_plan.h"

#include "emaster/bus/preop_probe.h"

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
            "  %s --interface IFACE --output FILE --acknowledge-preop\n\n"
            "探测会发送 EtherCAT 发现报文并请求 PRE-OP。它只读取 SDO，不映射 PDO、不配置 "
            "DC，也不请求 SAFE-OP 或 OP；关闭前会尝试恢复 INIT。\n",
            program, program);
}

static void print_interface(const char *name, const char *description, void *user_data)
{
    FILE *output = user_data;
    fprintf(output, "%s\t%s\n", name, description != NULL ? description : "");
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

    if (emaster_fingerprint_write_json(output, report, timestamp) != 0)
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
    const char *interface_name = NULL;
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
            fprintf(stderr, "%s.\n", probe_error(status));
            return 1;
        }
        return 0;
    }

    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--interface") == 0 && index + 1 < argc)
        {
            interface_name = argv[++index];
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

    if (interface_name == NULL || output_path == NULL || !acknowledge_preop)
    {
        usage(argv[0]);
        return 2;
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
    status = emaster_soem_preop_probe(interface_name, requests, request_count, &report);
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
        fprintf(stderr, "%s，接口：%s。\n", probe_error(status), interface_name);
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

    output_result = publish_report(output_path, &report, timestamp);
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
