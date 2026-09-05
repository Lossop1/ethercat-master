#ifndef EMASTER_AUDIT_RUN_AUDIT_H
#define EMASTER_AUDIT_RUN_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 阶段用于还原一次访问发生在总线生命周期的哪个位置，不承载控制策略。 */
typedef enum
{
    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION = 0,
    EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION,
    EMASTER_AUDIT_PHASE_OPERATION_REQUEST,
    EMASTER_AUDIT_PHASE_CYCLIC_OPERATION,
    EMASTER_AUDIT_PHASE_SAFE_STOP,
    EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC
} emaster_audit_phase_t;

typedef enum
{
    EMASTER_AUDIT_TRANSPORT_SDO = 0,
    EMASTER_AUDIT_TRANSPORT_PDO,
    EMASTER_AUDIT_TRANSPORT_ESC_REGISTER
} emaster_audit_transport_t;

typedef enum
{
    EMASTER_AUDIT_DIRECTION_READ = 0,
    EMASTER_AUDIT_DIRECTION_WRITE
} emaster_audit_direction_t;

typedef enum
{
    EMASTER_AUDIT_VALUE_UNSIGNED = 0,
    EMASTER_AUDIT_VALUE_SIGNED
} emaster_audit_value_kind_t;

/*
 * SDO 和 ESC 访问逐次保存；PDO 相同字段的连续同值样本会合并，并保留首次、末次交换号
 * 和样本数。这样既能精确还原控制字及目标变化，也不会为静止反馈反复保存相同记录。
 */
typedef struct
{
    uint64_t order;
    uint64_t first_exchange;
    uint64_t last_exchange;
    uint64_t sample_count;
    emaster_audit_phase_t phase;
    emaster_audit_transport_t transport;
    emaster_audit_direction_t direction;
    emaster_audit_value_kind_t value_kind;
    uint16_t slave_position;
    uint16_t index;
    uint8_t subindex;
    uint8_t bit_length;
    uint32_t bit_offset;
    uint8_t raw[8];
    uint8_t raw_size;
    bool succeeded;
    uint64_t unsigned_value;
    int64_t signed_value;
} emaster_audit_access_t;

typedef struct
{
    emaster_audit_access_t *accesses;
    size_t access_count;
    size_t access_capacity;
    uint64_t next_order;
    bool capacity_sealed;
    bool allocation_failed;
    /* 周期记录容量耗尽后只计数，不分配内存，也不阻断控制；报告显式标注截断。 */
    uint64_t omitted_pdo_samples;
} emaster_run_audit_t;

/* 初始化和析构只管理审计记录，不访问总线或文件系统。 */
void emaster_run_audit_init(emaster_run_audit_t *audit);
void emaster_run_audit_destroy(emaster_run_audit_t *audit);

/* 周期开始前预留容量并封存；封存后记录模块绝不在周期线程中重新分配内存。 */
bool emaster_run_audit_reserve(emaster_run_audit_t *audit, size_t capacity);
void emaster_run_audit_seal_capacity(emaster_run_audit_t *audit);
/* 仅在周期已停止后调用，允许后续 SDO 诊断继续保存完整记录。 */
void emaster_run_audit_end_cyclic(emaster_run_audit_t *audit);

/* 记录一次真实邮箱或 ESC 寄存器访问；raw 必须是总线上实际使用的字节序。 */
bool emaster_run_audit_record_access(
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    emaster_audit_transport_t transport,
    emaster_audit_direction_t direction,
    emaster_audit_value_kind_t value_kind,
    uint16_t slave_position,
    uint16_t index,
    uint8_t subindex,
    uint8_t bit_length,
    uint32_t bit_offset,
    uint64_t exchange,
    const void *raw,
    uint8_t raw_size,
    bool succeeded,
    uint64_t unsigned_value,
    int64_t signed_value);

/*
 * last_record 由每个过程映像字段独立持有，初值为 SIZE_MAX。记录器按数组下标直接
 * 查找上次样本，不扫描历史；下标在审计数组扩容后仍有效。只有连续交换、同阶段、
 * 同值且成功标志相同的样本才能合并。succeeded 表示该次过程数据交换的确认结果。
 */
bool emaster_run_audit_record_pdo(
    emaster_run_audit_t *audit,
    size_t *last_record,
    emaster_audit_phase_t phase,
    emaster_audit_direction_t direction,
    emaster_audit_value_kind_t value_kind,
    uint16_t slave_position,
    uint16_t index,
    uint8_t subindex,
    uint8_t bit_length,
    uint32_t bit_offset,
    uint64_t exchange,
    bool succeeded,
    uint64_t unsigned_value,
    int64_t signed_value);

#endif
