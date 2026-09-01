#ifndef EMASTER_PROTOCOL_PDO_LAYOUT_H
#define EMASTER_PROTOCOL_PDO_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    EMASTER_PDO_DISCOVERY_NOT_ATTEMPTED = 0,
    EMASTER_PDO_DISCOVERY_COMPLETE,
    EMASTER_PDO_DISCOVERY_INVALID_ARGUMENT,
    EMASTER_PDO_DISCOVERY_READ_FAILED,
    EMASTER_PDO_DISCOVERY_OUT_OF_MEMORY,
    EMASTER_PDO_DISCOVERY_INVALID_DESCRIPTOR
} emaster_pdo_discovery_status_t;

typedef struct
{
    uint8_t mapping_subindex;
    uint32_t raw_descriptor;
    uint16_t object_index;
    uint8_t object_subindex;
    uint8_t bit_length;
    uint32_t bit_offset;
} emaster_pdo_mapping_entry_t;

typedef struct
{
    uint16_t mapping_index;
    size_t entry_count;
    emaster_pdo_mapping_entry_t *entries;
    uint32_t bit_length;
} emaster_pdo_mapping_t;

typedef struct
{
    uint16_t assignment_index;
    size_t mapping_count;
    emaster_pdo_mapping_t *mappings;
    uint32_t bit_length;
} emaster_pdo_direction_layout_t;

typedef struct
{
    emaster_pdo_discovery_status_t status;
    uint16_t failed_index;
    uint8_t failed_subindex;
    emaster_pdo_direction_layout_t rx;
    emaster_pdo_direction_layout_t tx;
} emaster_pdo_layout_t;

/*
 * PDO 发现只依赖三个只读整数访问函数，不依赖 SOEM 或任何具体主站库。调用者负责保证读取
 * 发生在允许邮箱访问的 EtherCAT 状态，并把 user_data 传递给实际总线会话。
 */
typedef struct
{
    bool (*read_u8)(void *user_data, uint16_t index, uint8_t subindex, uint8_t *value);
    bool (*read_u16)(void *user_data, uint16_t index, uint8_t subindex, uint16_t *value);
    bool (*read_u32)(void *user_data, uint16_t index, uint8_t subindex, uint32_t *value);
    void *user_data;
} emaster_pdo_sdo_reader_t;

/*
 * 从标准分配对象开始逐层读取当前生效的完整映射。函数不写 SDO、不选择 PDO 方案，也不
 * 假设分配数量、映射索引或条目数量；失败位置记录在 result 中供上层诊断。
 */
emaster_pdo_discovery_status_t
emaster_pdo_layout_discover(const emaster_pdo_sdo_reader_t *reader,
                            emaster_pdo_layout_t *result);

/* 释放两方向的映射数组并清零结果；允许传入空指针。 */
void emaster_pdo_layout_destroy(emaster_pdo_layout_t *layout);

#endif
