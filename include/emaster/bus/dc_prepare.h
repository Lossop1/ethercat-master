#ifndef EMASTER_BUS_DC_PREPARE_H
#define EMASTER_BUS_DC_PREPARE_H

#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 该模块只负责一次受控的 PRE-OP 参数准备。调用结束时会关闭 Sync0 并请求恢复 INIT，
 * 因此它不是过程数据循环，也不会保持一个可继续交换 PDO 的 SOEM 上下文。
 */
typedef enum
{
    EMASTER_DC_PREPARE_OK = 0,
    EMASTER_DC_PREPARE_INVALID_ARGUMENT,
    EMASTER_DC_PREPARE_INTERFACE_OPEN_FAILED,
    EMASTER_DC_PREPARE_NO_SLAVES,
    EMASTER_DC_PREPARE_PREOP_NOT_REACHED,
    EMASTER_DC_PREPARE_TOPOLOGY_MISMATCH,
    EMASTER_DC_PREPARE_IDENTITY_MISMATCH,
    EMASTER_DC_PREPARE_PDO_MISMATCH,
    EMASTER_DC_PREPARE_DC_UNAVAILABLE,
    EMASTER_DC_PREPARE_SDO_WRITE_FAILED,
    EMASTER_DC_PREPARE_SDO_READBACK_FAILED,
    EMASTER_DC_PREPARE_DC_CONFIG_FAILED,
    EMASTER_DC_PREPARE_SYNC0_READBACK_FAILED,
    EMASTER_DC_PREPARE_RESTORE_INIT_FAILED
} emaster_dc_prepare_status_t;

typedef struct
{
    uint16_t position;
    bool identity_match;
    bool pdo_match;
    bool sm2_written;
    bool sm2_readback_match;
    bool sm3_written;
    bool sm3_readback_match;
    bool mode_written;
    bool mode_value_readback_match;
    bool mode_display_readback_match;
    bool mode_readback_match;
    int8_t mode_display;
    bool sync0_configured;
    bool sync0_readback_match;
    uint32_t sync0_cycle_ns;
    uint8_t sync0_activation;
} emaster_dc_prepare_axis_result_t;

typedef struct
{
    emaster_dc_prepare_status_t status;
    char interface_name[128];
    emaster_dc_prepare_axis_result_t *axes;
    size_t axis_count;
    bool sync0_disabled;
    bool restore_init_succeeded;
} emaster_dc_prepare_report_t;

/*
 * 按会话计划在所有实际从站上写入并读回 DC/CSP 参数。axis_storage 由调用者提供，容量必须
 * 覆盖计划中的全部轴；函数不分配持久化内存、不进入 SAFE-OP/OP，也不写控制字。
 */
emaster_dc_prepare_status_t emaster_soem_dc_prepare(
    const emaster_session_plan_t *plan,
    emaster_dc_prepare_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_dc_prepare_report_t *report);

#endif
