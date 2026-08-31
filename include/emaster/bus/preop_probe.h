#ifndef EMASTER_BUS_PREOP_PROBE_H
#define EMASTER_BUS_PREOP_PROBE_H

#include "emaster/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 这些值是诊断报告的内存容量上限，不是产品拓扑数量。探测前一次性分配有界内存，
 * 可避免外部从站数据导致无界分配；超过上限时必须失效关闭。
 */
#define EMASTER_PREOP_NAME_CAPACITY 128U
#define EMASTER_PREOP_OBJECT_NAME_CAPACITY 64U
#define EMASTER_PREOP_STRING_CAPACITY 128U
#define EMASTER_PREOP_MAX_SLAVES 200U
#define EMASTER_PREOP_MAX_SDO_REQUESTS 256U

typedef enum
{
    EMASTER_SDO_U8,
    EMASTER_SDO_I8,
    EMASTER_SDO_U16,
    EMASTER_SDO_U32,
    EMASTER_SDO_STRING
} emaster_sdo_value_type_t;

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    const char *name;
    emaster_sdo_value_type_t type;
} emaster_sdo_request_t;

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    char name[EMASTER_PREOP_OBJECT_NAME_CAPACITY];
    emaster_sdo_value_type_t type;
    bool ok;
    union
    {
        uint8_t u8;
        int8_t i8;
        uint16_t u16;
        uint32_t u32;
        char string[EMASTER_PREOP_STRING_CAPACITY];
    } value;
} emaster_sdo_read_t;

typedef struct
{
    /* position 使用 EtherCAT 总线顺序，从 1 开始且在一次报告中连续。 */
    uint16_t position;
    char name[EMASTER_PREOP_NAME_CAPACITY];
    emaster_slave_identity_t identity;
    uint16_t state;
    bool has_dc;
    bool has_coe;
    size_t sdo_read_count;
    emaster_sdo_read_t *sdo_reads;
} emaster_preop_slave_t;

typedef struct
{
    /* 报告及其动态数组由适配层创建，必须由 emaster_preop_report_destroy 释放。 */
    char interface_name[EMASTER_PREOP_NAME_CAPACITY];
    size_t slave_count;
    emaster_preop_slave_t *slaves;
    bool restore_init_succeeded;
} emaster_preop_report_t;

typedef enum
{
    EMASTER_PREOP_PROBE_OK = 0,
    EMASTER_PREOP_PROBE_INVALID_ARGUMENT,
    EMASTER_PREOP_PROBE_INTERFACE_ENUMERATION_FAILED,
    EMASTER_PREOP_PROBE_INTERFACE_OPEN_FAILED,
    EMASTER_PREOP_PROBE_NO_SLAVES,
    EMASTER_PREOP_PROBE_TOO_MANY_SLAVES,
    EMASTER_PREOP_PROBE_PREOP_NOT_REACHED,
    EMASTER_PREOP_PROBE_SDO_READ_FAILED,
    EMASTER_PREOP_PROBE_OUT_OF_MEMORY,
    EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED
} emaster_preop_probe_status_t;

typedef void (*emaster_interface_visitor_t)(const char *name, const char *description,
                                            void *user_data);

/* 枚举本机可供操作者选择的接口；此函数不发送 EtherCAT 帧。 */
emaster_preop_probe_status_t
emaster_soem_visit_interfaces(emaster_interface_visitor_t visitor, void *user_data);

/*
 * 在明确指定的接口上执行一次受限探测。实现最高只请求 PRE-OP，只读 SII/SDO，
 * 不映射 PDO、不配置 DC，并在结束前尝试恢复 INIT。
 */
emaster_preop_probe_status_t
emaster_soem_preop_probe(const char *interface_name, const emaster_sdo_request_t *requests,
                         size_t request_count, emaster_preop_report_t *report);

/* 释放报告拥有的数组并清零结构；允许传入空指针。 */
void emaster_preop_report_destroy(emaster_preop_report_t *report);

#endif
