#define _POSIX_C_SOURCE 200809L

#include "console.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *probe_status_text(emaster_preop_probe_status_t status)
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
            return "一个或多个 CoE 从站的诊断 SDO 读取全部失败";
        case EMASTER_PREOP_PROBE_PDO_DISCOVERY_FAILED:
            return "一个或多个 CoE 从站的 PDO 映射读取不完整";
        case EMASTER_PREOP_PROBE_OUT_OF_MEMORY:
            return "无法为指纹报告分配内存";
        case EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED:
            return "一个或多个从站未确认恢复 INIT";
        case EMASTER_PREOP_PROBE_OK:
            return "成功";
    }
    return "未知 PRE-OP 探测错误";
}

emaster_command_t emaster_console_parse_command(int argc, char **argv)
{
    emaster_command_t result = {EMASTER_COMMAND_INVALID, NULL};

    if (argc == 2 && strcmp(argv[1], "interfaces") == 0)
    {
        result.kind = EMASTER_COMMAND_INTERFACES;
    }
    else if (argc == 2 && strcmp(argv[1], "deployments") == 0)
    {
        result.kind = EMASTER_COMMAND_DEPLOYMENTS;
    }
    else if (argc == 3 && strcmp(argv[1], "capture") == 0)
    {
        result.kind = EMASTER_COMMAND_CAPTURE;
        result.output_path = argv[2];
    }
    return result;
}

void emaster_console_print_usage(const char *program)
{
    fprintf(stderr,
            "用法：\n"
            "  %s interfaces\n"
            "  %s deployments\n"
            "  sudo %s capture OUTPUT_FILE\n\n"
            "探测会发送 EtherCAT 发现报文并请求 PRE-OP。它只读取 SDO，不映射 PDO、不配置 "
            "DC，也不请求 SAFE-OP 或 OP；关闭前会尝试恢复 INIT。\n",
            program, program, program);
}

void emaster_console_print_interface(const char *name, const char *description,
                                     void *user_data)
{
    FILE *output = user_data;
    fprintf(output, "%s\t%s\n", name, description != NULL ? description : "");
}

void emaster_console_print_deployment(const emaster_deployment_config_t *deployment)
{
    printf("%s\t主机=%s\t接口=%s\t拓扑=%s\n", deployment->deployment_id,
           deployment->hostname, deployment->ethercat_interface,
           deployment->topology->topology_id);
}

bool emaster_console_confirm_preop(const emaster_deployment_config_t *deployment)
{
    char input[32];
    size_t length;

    if (deployment == NULL || !isatty(STDIN_FILENO))
    {
        fputs("PRE-OP 采集必须在交互终端中确认。\n", stderr);
        return false;
    }
    fprintf(stderr,
            "即将在主机 %s 的接口 %s 上请求 PRE-OP，预期拓扑为 %s。\n"
            "输入 PRE-OP 继续：",
            deployment->hostname, deployment->ethercat_interface,
            deployment->topology->topology_id);
    if (fflush(stderr) != 0 || fgets(input, sizeof(input), stdin) == NULL)
    {
        return false;
    }
    length = strcspn(input, "\r\n");
    input[length] = '\0';
    return strcmp(input, "PRE-OP") == 0;
}

void emaster_console_print_message(emaster_message_id_t message, const char *detail,
                                   int system_error)
{
    const char *text = detail != NULL ? detail : "";
    const char *error_text = system_error != 0 ? strerror(system_error) : "";

    switch (message)
    {
        case EMASTER_MESSAGE_DEPLOYMENT_UNAVAILABLE:
            fputs("当前主机没有唯一且完整的部署配置，拒绝打开 EtherCAT 接口。\n", stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_EXISTS:
            fprintf(stderr, "拒绝覆盖已有指纹文件：%s\n", text);
            break;
        case EMASTER_MESSAGE_SDO_PLAN_FAILED:
            fputs("无法生成诊断 SDO 读取计划。\n", stderr);
            break;
        case EMASTER_MESSAGE_PREOP_NOT_CONFIRMED:
            fputs("未确认 PRE-OP，未发送 EtherCAT 报文。\n", stderr);
            break;
        case EMASTER_MESSAGE_TIMESTAMP_FAILED:
            fputs("无法生成可信的 UTC 时间戳，未发送 EtherCAT 报文。\n", stderr);
            break;
        case EMASTER_MESSAGE_EMPTY_REPORT:
            fputs("探测没有生成可用的从站报告。\n", stderr);
            break;
        case EMASTER_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH:
            fputs("物理从站身份或 PDO 映射与部署配置不一致，拒绝发布指纹。\n", stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_PATH_TOO_LONG:
            fputs("输出路径过长。\n", stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_CREATE_FAILED:
            fprintf(stderr, "无法创建指纹临时文件：%s\n", error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_OPEN_FAILED:
            fprintf(stderr, "无法打开指纹输出流：%s\n", error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_WRITE_FAILED:
            fputs("写入指纹失败。\n", stderr);
            break;
        case EMASTER_MESSAGE_OUTPUT_FLUSH_FAILED:
            fprintf(stderr, "刷新指纹文件失败：%s\n", error_text);
            break;
        case EMASTER_MESSAGE_OUTPUT_PUBLISH_FAILED:
            fprintf(stderr, "发布指纹文件失败：%s\n", error_text);
            break;
        case EMASTER_MESSAGE_DIAGNOSTIC_REPORT_SAVED:
            fputs("已保存诊断报告，但该报告不能作为通过证据。\n", stderr);
            break;
        case EMASTER_MESSAGE_INCOMPLETE_REPORT_REJECTED:
            fputs("未发布 PDO 映射不完整的指纹证据。\n", stderr);
            break;
    }
}

void emaster_console_print_probe_status(emaster_preop_probe_status_t status,
                                        const char *interface_name, bool warning)
{
    const char *prefix = warning ? "警告" : "错误";
    if (interface_name != NULL && interface_name[0] != '\0')
    {
        fprintf(stderr, "%s：%s，接口：%s。\n", prefix, probe_status_text(status),
                interface_name);
    }
    else
    {
        fprintf(stderr, "%s：%s。\n", prefix, probe_status_text(status));
    }
}

void emaster_console_print_pdo_failure(const emaster_pdo_layout_t *layout,
                                       uint16_t position)
{
    if (layout == NULL || layout->status == EMASTER_PDO_DISCOVERY_COMPLETE)
    {
        return;
    }
    fprintf(stderr,
            "从站位置 %u 的 PDO 发现失败，状态=%u，最后访问对象=0x%04X:%02X。\n",
            (unsigned int)position, (unsigned int)layout->status,
            layout->failed_index, (unsigned int)layout->failed_subindex);
}
