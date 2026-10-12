#ifndef EMASTER_SOEM_COMMON_H
#define EMASTER_SOEM_COMMON_H

#include "emaster/audit/run_audit.h"
#include "emaster/protocol/pdo_layout.h"
#include "emaster/catalog/slave_profile.h"

#include "soem/soem.h"

#include <stdbool.h>
#include <stdint.h>

/* SOEM 访问只在 src/bus/soem 内部共享；该头文件不属于公共主站接口。 */
typedef struct
{
    ecx_contextt *context;
    uint16_t slave;
    emaster_run_audit_t *audit;
    emaster_audit_phase_t phase;
    uint64_t exchange;
    /*
     * 本次邮箱访问的等待上限（µs）。emaster_soem_sdo_context_init 默认填
     * EC_TIMEOUTRXM（700 ms），调用者可用 emaster_soem_sdo_set_timeout 收紧。
     *
     * 为什么必须能改：**读失败不是立刻失败，是先等满这个超时才失败。**
     * 一个不可达的从站，12 台 x 3 次操作就能让停机卡 25 秒。观测线程早就因为
     * 这个理由自己调 ecx_SDOread(..., 30000) 绕开了这个函数（见
     * session_observer_thread.c 顶部注释），但那等于把同一件事做了两遍。
     * 停机时的失能核对是第二个需要收紧的调用者：它宁可"30 ms 读不到算读不到"，
     * 也不能拿 25 秒去换一个已知读不到的答案。
     */
    int timeout_us;
} emaster_soem_sdo_reader_context_t;

/* 收紧本上下文的邮箱超时。us <= 0 或大于 EC_TIMEOUTRXM 时按 EC_TIMEOUTRXM 处理。 */
void emaster_soem_sdo_set_timeout(emaster_soem_sdo_reader_context_t *reader, int us);

/* 建立一次从站邮箱访问上下文；审计指针可以为空，供非控制诊断工具复用。 */
void emaster_soem_sdo_context_init(
    emaster_soem_sdo_reader_context_t *reader,
    ecx_contextt *context,
    uint16_t slave,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint64_t exchange);

/* 请求全体从站恢复 INIT，并返回状态确认结果。 */
bool emaster_soem_restore_init(ecx_contextt *context);

/* 在打开 SOEM 原始套接字前检查物理接口是否已检测到链路。 */
bool emaster_soem_interface_carrier(const char *interface_name);

/* 等待 SOEM 发现后的全体从站进入 PRE-OP。 */
bool emaster_soem_wait_preop(ecx_contextt *context);

/* 以精确字节宽度读取 CoE 数值对象，自动处理 EtherCAT 字节序。 */
bool emaster_soem_read_u8(void *user_data, uint16_t index, uint8_t subindex,
                          uint8_t *value);
bool emaster_soem_read_u16(void *user_data, uint16_t index, uint8_t subindex,
                           uint16_t *value);
bool emaster_soem_read_u32(void *user_data, uint16_t index, uint8_t subindex,
                           uint32_t *value);
bool emaster_soem_read_i8(void *user_data, uint16_t index, uint8_t subindex,
                          int8_t *value);
bool emaster_soem_read_i16(void *user_data, uint16_t index, uint8_t subindex,
                           int16_t *value);
bool emaster_soem_read_i32(void *user_data, uint16_t index, uint8_t subindex,
                           int32_t *value);
bool emaster_soem_write_u8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, uint8_t value);
bool emaster_soem_write_u16(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint16_t value);
bool emaster_soem_write_u32(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint32_t value);
bool emaster_soem_write_i8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, int8_t value);
bool emaster_soem_write_i16(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, int16_t value);
bool emaster_soem_write_i32(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, int32_t value);

/* 发现指定从站当前生效的完整 PDO 布局；调用者负责析构结果。 */
bool emaster_soem_discover_pdo_layout(ecx_contextt *context, uint16_t slave,
                                      emaster_pdo_layout_t *layout);
bool emaster_soem_discover_pdo_layout_recorded(
    emaster_soem_sdo_reader_context_t *reader,
    emaster_pdo_layout_t *layout);

typedef struct
{
    uint16_t failed_index;
    uint8_t failed_subindex;
    bool abort_code_available;
    uint32_t abort_code;
} emaster_soem_pdo_assignment_result_t;

/* 在 PRE-OP 选择设备模块并按方案分配 RxPDO/TxPDO，所有写入均读回确认。 */
bool emaster_soem_assign_pdo_set(ecx_contextt *context, uint16_t slave,
                                 const emaster_pdo_set_profile_t *pdo_set,
                                 emaster_soem_pdo_assignment_result_t *result);
bool emaster_soem_assign_pdo_set_recorded(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_pdo_set_profile_t *pdo_set,
    emaster_soem_pdo_assignment_result_t *result);

#endif
