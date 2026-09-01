#ifndef EMASTER_BUS_CIA_PREFLIGHT_H
#define EMASTER_BUS_CIA_PREFLIGHT_H

#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * CiA 402 前置流程只验证过程数据和状态反馈通路，不执行状态机控制字操作。
 * 调用结束时会关闭 Sync0，并请求所有从站恢复 INIT。
 */
typedef enum
{
    EMASTER_CIA_PREFLIGHT_OK = 0,
    EMASTER_CIA_PREFLIGHT_INVALID_ARGUMENT,
    EMASTER_CIA_PREFLIGHT_INTERFACE_OPEN_FAILED,
    EMASTER_CIA_PREFLIGHT_INTERFACE_NOT_READY,
    EMASTER_CIA_PREFLIGHT_NO_SLAVES,
    EMASTER_CIA_PREFLIGHT_TOPOLOGY_MISMATCH,
    EMASTER_CIA_PREFLIGHT_PREOP_NOT_REACHED,
    EMASTER_CIA_PREFLIGHT_IDENTITY_MISMATCH,
    EMASTER_CIA_PREFLIGHT_PDO_MISMATCH,
    EMASTER_CIA_PREFLIGHT_SDO_WRITE_FAILED,
    EMASTER_CIA_PREFLIGHT_SDO_READBACK_FAILED,
    EMASTER_CIA_PREFLIGHT_PROCESS_MAP_FAILED,
    EMASTER_CIA_PREFLIGHT_SAFE_OP_NOT_REACHED,
    EMASTER_CIA_PREFLIGHT_OUT_OF_MEMORY,
    EMASTER_CIA_PREFLIGHT_DC_CONFIG_FAILED,
    EMASTER_CIA_PREFLIGHT_SYNC0_CONFIG_FAILED,
    EMASTER_CIA_PREFLIGHT_INITIAL_WKC_FAILED,
    EMASTER_CIA_PREFLIGHT_OP_NOT_REACHED,
    EMASTER_CIA_PREFLIGHT_FEEDBACK_INVALID,
    EMASTER_CIA_PREFLIGHT_RESTORE_INIT_FAILED
} emaster_cia_preflight_status_t;

typedef struct
{
    uint16_t position;
    bool identity_match;
    bool pdo_match;
    bool process_map_match;
    bool output_initialized;
    bool input_decoded;
    bool mode_display_match;
    int8_t mode_display;
    uint16_t status_word;
} emaster_cia_preflight_axis_result_t;

typedef struct
{
    emaster_cia_preflight_status_t status;
    char interface_name[128];
    emaster_cia_preflight_axis_result_t *axes;
    size_t axis_count;
    size_t io_map_size;
    uint16_t expected_wkc;
    int actual_wkc;
    bool safe_op_reached;
    bool op_reached;
    bool sync0_disabled;
    bool restore_init_succeeded;
} emaster_cia_preflight_report_t;

/*
 * 建立一次受控的 SAFE-OP/OP 首帧过程数据会话。输出只包含零控制字、零目标值和配置指定
 * 的模式值；函数不执行 CiA 402 状态转换，不使能电机，不产生运动命令，也不分配持久化内存。
 */
emaster_cia_preflight_status_t emaster_soem_cia_preflight(
    const emaster_session_plan_t *plan,
    emaster_cia_preflight_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_cia_preflight_report_t *report);

#endif
