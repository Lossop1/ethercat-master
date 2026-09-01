#ifndef EMASTER_PROTOCOL_PDO_CODEC_H
#define EMASTER_PROTOCOL_PDO_CODEC_H

#include "emaster/protocol/pdo_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 编解码当前以最多 64 位的整数对象为边界；更宽对象必须由上层另行定义。 */
#define EMASTER_PDO_CODEC_MAX_VALUE_BITS UINT8_C(64)

typedef enum
{
    EMASTER_PDO_CODEC_VALUE_UNSIGNED = 0,
    EMASTER_PDO_CODEC_VALUE_SIGNED,
    /* 填充项占用实际过程数据位，但不是可由控制逻辑赋值的对象。 */
    EMASTER_PDO_CODEC_VALUE_PADDING
} emaster_pdo_codec_value_kind_t;

/* 字段类型只描述原始整数的符号性或填充属性，不包含单位、缩放或运动语义。 */
typedef struct
{
    emaster_pdo_codec_value_kind_t kind;
} emaster_pdo_codec_field_t;

typedef struct
{
    emaster_pdo_codec_value_kind_t kind;
    union
    {
        /* 填充项解码后也使用该成员保存实际位值，便于上层诊断非零填充。 */
        uint64_t unsigned_value;
        int64_t signed_value;
    } value;
} emaster_pdo_codec_value_t;

typedef enum
{
    EMASTER_PDO_CODEC_OK = 0,
    EMASTER_PDO_CODEC_INVALID_ARGUMENT,
    EMASTER_PDO_CODEC_INVALID_LAYOUT,
    EMASTER_PDO_CODEC_FIELD_COUNT_MISMATCH,
    EMASTER_PDO_CODEC_UNSUPPORTED_BIT_LENGTH,
    EMASTER_PDO_CODEC_BUFFER_TOO_SMALL,
    EMASTER_PDO_CODEC_VALUE_KIND_MISMATCH,
    EMASTER_PDO_CODEC_VALUE_OUT_OF_RANGE
} emaster_pdo_codec_status_t;

/* 计算方向中展开后的条目数量；布局无效或 size_t 溢出时返回 SIZE_MAX。 */
size_t emaster_pdo_codec_field_count(const emaster_pdo_direction_layout_t *layout);

/*
 * 检查布局的位偏移、映射长度、方向总长度和字段类型。该函数不访问总线、不分配内存，
 * 也不要求提供过程数据缓冲区；调用者应先确认布局已经与设备目录匹配。
 */
emaster_pdo_codec_status_t
emaster_pdo_codec_validate(const emaster_pdo_direction_layout_t *layout,
                           const emaster_pdo_codec_field_t *fields, size_t field_count);

/* 根据位长度计算所需字节数；该计算对所有 uint32_t 位长度均有效。 */
size_t emaster_pdo_codec_byte_length(uint32_t bit_length);

/*
 * 按布局顺序把原始整数值写入过程数据缓冲区。位偏移按 EtherCAT 低位优先规则解释；
 * 填充项必须以 PADDING 类型和值零提供。函数会先完成全部校验，成功后才清零并写入
 * 所需字节，不会留下部分编码结果；失败时不修改缓冲区。
 */
emaster_pdo_codec_status_t
emaster_pdo_codec_encode(const emaster_pdo_direction_layout_t *layout,
                         const emaster_pdo_codec_field_t *fields, size_t field_count,
                         const emaster_pdo_codec_value_t *values, size_t value_count,
                         uint8_t *buffer, size_t buffer_capacity);

/*
 * 按布局顺序从过程数据缓冲区读取原始整数值。输出值的 kind 与字段描述一致；函数不
 * 修改输入缓冲区，失败时也不修改输出值。PADDING 类型的 unsigned_value 保留实际位值，
 * 供上层判断填充是否为零；函数不把原始整数解释为角度、速度、力矩或其他工程单位。
 */
emaster_pdo_codec_status_t
emaster_pdo_codec_decode(const emaster_pdo_direction_layout_t *layout,
                         const emaster_pdo_codec_field_t *fields, size_t field_count,
                         const uint8_t *buffer, size_t buffer_length,
                         emaster_pdo_codec_value_t *values, size_t value_capacity);

#endif
