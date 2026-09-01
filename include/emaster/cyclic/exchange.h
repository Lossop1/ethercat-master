#ifndef EMASTER_CYCLIC_EXCHANGE_H
#define EMASTER_CYCLIC_EXCHANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 一次完整过程数据交换的结果；失败时不得把部分帧视为有效。 */
typedef enum
{
    EMASTER_CYCLIC_OK = 0,
    EMASTER_CYCLIC_INVALID_ARGUMENT,
    EMASTER_CYCLIC_AXIS_COUNT_MISMATCH,
    EMASTER_CYCLIC_SEQUENCE_NOT_MONOTONIC,
    EMASTER_CYCLIC_DEADLINE_EXPIRED,
    EMASTER_CYCLIC_TRANSPORT_FAILED,
    EMASTER_CYCLIC_WORK_COUNTER_MISMATCH
} emaster_cyclic_status_t;

/*
 * 总线适配层在初始化阶段绑定一次交换回调；周期路径只能使用预先准备好的缓冲区，
 * 回调不得执行 SDO、状态扫描、动态分配、文件或日志操作。
 */
typedef bool (*emaster_cyclic_exchange_callback_t)(
    void *user_data,
    const uint8_t *output_bytes,
    size_t output_length,
    uint8_t *input_bytes,
    size_t input_capacity,
    int *actual_wkc);

typedef struct
{
    emaster_cyclic_exchange_callback_t exchange;
    void *user_data;
} emaster_cyclic_transport_t;

/* 周期交换器只保存轴数、预期 WKC 和序号，不拥有任何缓冲区。 */
typedef struct
{
    size_t axis_count;
    uint16_t expected_wkc;
    uint64_t last_sequence;
    bool has_last_sequence;
} emaster_cyclic_exchange_t;

/* 调用者提供的完整原始过程数据帧；数组在整个 step 调用期间必须保持稳定。 */
typedef struct
{
    uint64_t sequence;
    uint64_t deadline_ns;
    size_t axis_count;
    const uint8_t *output_bytes;
    size_t output_length;
    uint8_t *input_bytes;
    size_t input_capacity;
} emaster_cyclic_frame_t;

typedef struct
{
    emaster_cyclic_status_t status;
    uint64_t sequence;
    int actual_wkc;
    bool transport_succeeded;
    bool work_counter_match;
} emaster_cyclic_result_t;

/* 在周期线程启动前配置交换器；axis_count 和 expected_wkc 必须来自已确认的运行时映像。 */
bool emaster_cyclic_exchange_init(emaster_cyclic_exchange_t *exchange,
                                   size_t axis_count, uint16_t expected_wkc);

/*
 * 执行一个完整周期交换。now_ns 由外部单调时钟提供；本函数不休眠、不读取时钟、不编码
 * PDO，也不改变输入输出缓冲区的所有权。任意门禁失败时不会调用总线回调。
 */
emaster_cyclic_status_t emaster_cyclic_exchange_step(
    emaster_cyclic_exchange_t *exchange,
    const emaster_cyclic_transport_t *transport,
    const emaster_cyclic_frame_t *frame,
    uint64_t now_ns,
    emaster_cyclic_result_t *result);

#endif
