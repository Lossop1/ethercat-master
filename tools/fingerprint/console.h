#ifndef EMASTER_TOOL_FINGERPRINT_CONSOLE_H
#define EMASTER_TOOL_FINGERPRINT_CONSOLE_H

#include "emaster/bus/preop_probe.h"
#include "emaster/config/runtime_config.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum
{
    EMASTER_COMMAND_INVALID = 0,
    EMASTER_COMMAND_INTERFACES,
    EMASTER_COMMAND_DEPLOYMENTS,
    EMASTER_COMMAND_CAPTURE
} emaster_command_kind_t;

typedef struct
{
    emaster_command_kind_t kind;
    const char *output_path;
} emaster_command_t;

typedef enum
{
    EMASTER_MESSAGE_DEPLOYMENT_UNAVAILABLE = 0,
    EMASTER_MESSAGE_OUTPUT_EXISTS,
    EMASTER_MESSAGE_SDO_PLAN_FAILED,
    EMASTER_MESSAGE_PREOP_NOT_CONFIRMED,
    EMASTER_MESSAGE_TIMESTAMP_FAILED,
    EMASTER_MESSAGE_EMPTY_REPORT,
    EMASTER_MESSAGE_TOPOLOGY_OR_PDO_MISMATCH,
    EMASTER_MESSAGE_OUTPUT_PATH_TOO_LONG,
    EMASTER_MESSAGE_OUTPUT_CREATE_FAILED,
    EMASTER_MESSAGE_OUTPUT_OPEN_FAILED,
    EMASTER_MESSAGE_OUTPUT_WRITE_FAILED,
    EMASTER_MESSAGE_OUTPUT_FLUSH_FAILED,
    EMASTER_MESSAGE_OUTPUT_PUBLISH_FAILED,
    EMASTER_MESSAGE_DIAGNOSTIC_REPORT_SAVED,
    EMASTER_MESSAGE_INCOMPLETE_REPORT_REJECTED
} emaster_message_id_t;

/* 命令词和参数个数只在展示边界解释；工程参数不从命令行进入。 */
emaster_command_t emaster_console_parse_command(int argc, char **argv);
void emaster_console_print_usage(const char *program);

void emaster_console_print_interface(const char *name, const char *description,
                                     void *user_data);
void emaster_console_print_deployment(const emaster_deployment_config_t *deployment);

/* 逐次交互确认只属于操作界面，不进入 EtherCAT 或协议模块。 */
bool emaster_console_confirm_preop(const emaster_deployment_config_t *deployment);

void emaster_console_print_message(emaster_message_id_t message, const char *detail,
                                   int system_error);
void emaster_console_print_probe_status(emaster_preop_probe_status_t status,
                                        const char *interface_name, bool warning);
void emaster_console_print_pdo_failure(const emaster_pdo_layout_t *layout,
                                       uint16_t position);

#endif
