#ifndef EMASTER_BUS_SOEM_CIA_PROCESS_IMAGE_H
#define EMASTER_BUS_SOEM_CIA_PROCESS_IMAGE_H

#include "emaster/audit/run_audit.h"
#include "emaster/protocol/pdo_codec.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 单轴过程映像保存已核对布局对应的字段位置和预分配编解码存储。 */
typedef struct
{
    emaster_pdo_layout_t layout;
    emaster_pdo_codec_field_t *rx_fields;
    emaster_pdo_codec_field_t *tx_fields;
    emaster_pdo_codec_value_t *rx_values;
    emaster_pdo_codec_value_t *tx_values;
    /* 与字段一一对应的审计游标，使每周期审计工作量仅取决于当前 PDO 字段数。 */
    size_t *rx_audit_records;
    size_t *tx_audit_records;
    size_t rx_field_count;
    size_t tx_field_count;
    size_t rx_mode_ordinal;
    size_t rx_control_ordinal;
    size_t tx_mode_ordinal;
    size_t tx_status_ordinal;
    size_t rx_target_position_ordinal;
    size_t tx_actual_position_ordinal;
    /* 动态模块通过 PDO 提供模式字段；固定模块没有该字段。 */
    bool rx_mode_available;
    bool tx_mode_available;
    bool mode_read_pending;
    bool sync0_configured;
} emaster_cia_process_image_t;

/* 根据实际发现布局和设备目录建立字段绑定；不会访问总线。 */
bool emaster_cia_process_image_init(const emaster_session_axis_plan_t *axis,
                                    emaster_cia_process_image_t *image);

/* 初始化完整 RxPDO，控制字和运动目标均为零，模式来自运行方案。 */
bool emaster_cia_process_image_prepare_output(
    const emaster_session_axis_plan_t *axis,
    emaster_cia_process_image_t *image,
    uint8_t *output,
    size_t output_capacity);

/* 更新控制字和 CSP 保持目标，并重新编码完整 RxPDO。 */
bool emaster_cia_process_image_update_output(
    const emaster_session_axis_plan_t *axis,
    emaster_cia_process_image_t *image,
    uint16_t control_word,
    int32_t target_position,
    uint8_t *output,
    size_t output_capacity);

/* 解码状态字、模式显示和 CSP 实际位置；未使用 CSP 时位置返回零。 */
bool emaster_cia_process_image_decode_input(
    emaster_cia_process_image_t *image,
    const uint8_t *input,
    size_t input_length,
    int8_t *mode_display,
    uint16_t *status_word,
    int32_t *actual_position);

/* 交换结束后记录 RxPDO 及其 WKC 确认结果，成功解码后记录 TxPDO。 */
bool emaster_cia_process_image_audit_output(
    const emaster_cia_process_image_t *image,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint16_t slave_position,
    uint64_t exchange,
    bool succeeded);
bool emaster_cia_process_image_audit_input(
    const emaster_cia_process_image_t *image,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint16_t slave_position,
    uint64_t exchange);

/* 释放初始化阶段分配的数组和 PDO 布局。 */
void emaster_cia_process_image_destroy(emaster_cia_process_image_t *images,
                                       size_t count);

#endif
