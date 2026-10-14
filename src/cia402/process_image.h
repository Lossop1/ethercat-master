#ifndef EMASTER_BUS_SOEM_CIA_PROCESS_IMAGE_H
#define EMASTER_BUS_SOEM_CIA_PROCESS_IMAGE_H

#include "emaster/audit/run_audit.h"
#include "emaster/protocol/pdo_codec.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 运行方案为某个 RxPDO 对象声明的固定值，按字段序号与字段一一对应。 */
typedef struct
{
    bool present;
    int64_t value;
} emaster_cia_declared_output_t;

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
    /*
     * 方案为 RxPDO 里某个对象声明的固定值（safeop_to_op_sdo_writes），按字段序号存放。
     *
     * 单独存一份是因为"SAFE-OP 进 OP 时写一次 SDO"不足以保证对象在运行期是这个值：
     * 设备把同一个对象也映射进 RxPDO 时，每拍送出去的周期值会立刻把它顶掉。台架实测
     * 就是这么一台：0x6072 的 SDO 写读回来是对的，随后的周期写恒为零（10732 拍），
     * OP 期间从驱动器读回的最大力矩也就是零。所以声明的值要逐拍写出去。
     *
     * present 为 false 的字段照旧按填充写零；本模式的目标字段不在这里，它由目标通道给。
     */
    emaster_cia_declared_output_t *rx_declared;
    size_t rx_field_count;
    size_t tx_field_count;
    size_t rx_mode_ordinal;
    size_t rx_control_ordinal;
    size_t tx_mode_ordinal;
    size_t tx_status_ordinal;
    size_t rx_target_position_ordinal;
    size_t tx_actual_position_ordinal;
    size_t rx_target_velocity_ordinal;
    size_t tx_actual_velocity_ordinal;
    size_t rx_target_torque_ordinal;
    size_t tx_actual_torque_ordinal;
    size_t tx_actual_current_ordinal;
    size_t tx_dc_link_voltage_ordinal;
    /*
     * 驱动器错误码 0x603F 在 TxPDO 里的位置。表 0x1A00 的第 5 条就是它，凡按标准状态
     * 通道建的从站都有；本 PDO 里没有时保持 SIZE_MAX。
     */
    size_t tx_error_code_ordinal;
    /* 动态模块通过 PDO 提供模式字段；固定模块没有该字段。 */
    bool rx_mode_available;
    bool tx_mode_available;
    int8_t configured_mode;
    /*
     * 本轴配置的模式：目标通道收的那个数是什么量。解码要挑哪一路反馈、编码要往哪个
     * 目标字段写，都按它分。存下来是因为 decode_input 只拿到映像、拿不到轴的计划。
     */
    emaster_mode_quantity_t configured_quantity;
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

/* 更新控制字和所选模式的目标原始值，并重新编码完整 RxPDO。 */
bool emaster_cia_process_image_update_output(
    const emaster_session_axis_plan_t *axis,
    emaster_cia_process_image_t *image,
    uint16_t control_word,
    int32_t target_value,
    uint8_t *output,
    size_t output_capacity);

/* 解码状态字、模式显示和所选模式的反馈原始值；调用者按模式解释该值。 */
bool emaster_cia_process_image_decode_input(
    emaster_cia_process_image_t *image,
    const uint8_t *input,
    size_t input_length,
    int8_t *mode_display,
    uint16_t *status_word,
    int32_t *actual_value);

/* TxPDO 里的三路反馈，与当前运行模式无关。 */
typedef enum
{
    EMASTER_CIA_FEEDBACK_POSITION = 0,
    EMASTER_CIA_FEEDBACK_VELOCITY = 1,
    EMASTER_CIA_FEEDBACK_TORQUE = 2
} emaster_cia_feedback_t;

/*
 * 读取上一次解码留下的某一路反馈原始值。字段不在本 PDO 布局里、类型不是有符号、
 * 或超出该物理量的取值范围时返回 false，*value 不动。
 *
 * 与 decode_input 的区别在"谁来挑"：那个按运行模式挑一路（模式 10 只给力矩），
 * 于是 CST 下位置根本读不出来；这个按 PDO 里实际有的字段给，模式不参与。
 */
bool emaster_cia_process_image_feedback(const emaster_cia_process_image_t *image,
                                        emaster_cia_feedback_t feedback,
                                        int32_t *value);

/*
 * 上一次解码留下的驱动器错误码（0x603F），0 = 无故障。
 *
 * 与上面几路反馈同一个来路：都是过程映像里逐拍带回来的真实读数，不是慢速轮询的
 * 缓存。分开读的理由是它判的东西不一样——这个答的是"驱动器此刻自报什么故障"，而
 * 状态字的故障位只答"驱动器的 CiA 402 状态机在不在 Fault"。MT 台架实测这两者会分家：
 * 驱动器内部报故障时状态字 bit3 一直是 0，0x6061 却回 0。
 *
 * 本 PDO 布局里没有 0x603F 这条时返回 false，*value 不动。
 */
bool emaster_cia_process_image_drive_error_code(const emaster_cia_process_image_t *image,
                                                uint16_t *value);

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
