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
    /*
     * 封存那一刻的**周期阶段**容量水线，只增不减。cycle 阶段的追加上限看它，不看
     * access_capacity——预留总量里留出的一部分是给停机诊断的，周期阶段不许吃掉它。
     */
    size_t sealed_capacity;
    /*
     * 预留额度用完那一刻定下的总容量，此后**任何路径都不得超过它**。
     * 这是"周期路径里不再发生分配"的硬保证：解封之后（停机阶段）追加到 access_capacity
     * 为止就不再扩数组，多出来的只计数。0 表示还没做过预留，此时按需倍增（只在会话
     * 建立阶段的 SDO 配置记录里发生，那时还没有周期）——record_access / record_pdo 的
     * "满了只计数"预判同样以它非 0 为前提，否则预留之前 `0 >= 0` 会把配置阶段的记录
     * 全部当成溢出丢掉。
     */
    size_t final_capacity;
    /* 周期段（sealed_capacity 这条水线）吃满后只计数，不分配内存，也不阻断控制；
     * 报告与控制台都显式标注截断。周期阶段记的绝大多数是 PDO 字段样本，故沿用此名。 */
    uint64_t omitted_pdo_samples;
    /* 解封之后连预留总量（final_capacity）也用尽时同样只计数。两笔分开记，才能从
     * 报告里看出是"周期段被吃光"还是"停机预留也被吃光"。 */
    uint64_t omitted_final_samples;
    /* 0 表示不设上限。由 apply_capacity_limit 写入，由 clamp_capacity 在封存前生效。 */
    size_t capacity_limit;
} emaster_run_audit_t;

/* 初始化和析构只管理审计记录，不访问总线或文件系统。 */
void emaster_run_audit_init(emaster_run_audit_t *audit);
void emaster_run_audit_destroy(emaster_run_audit_t *audit);

/*
 * 周期开始前一次性预留容量。**只此一次**：预留之后 final_capacity 定死，记录模块
 * 从此（含解封后的停机阶段）不再扩数组——周期路径里一次 realloc 就是几毫秒，
 * 实测 84.6 MB 的块翻倍让内核花了 3.757 ms，正好落在周期尾部，五轴因此全掉出 OP。
 */
bool emaster_run_audit_reserve(emaster_run_audit_t *audit, size_t capacity);
/*
 * 封存周期阶段的追加上限。cyclic_capacity 应等于"周期阶段算出来的容量"，小于预留
 * 总量——差额留给停机诊断，周期阶段吃不到它。传大于 access_capacity 的值按
 * access_capacity 处理（预留被上限压小时会走到这一支）。
 * 传入 0 表示周期阶段一条都不许追加（仍只计数）。
 */
void emaster_run_audit_seal_cyclic(emaster_run_audit_t *audit, size_t cyclic_capacity);
/*
 * 读 EMASTER_AUDIT_MAX_ACCESSES 并记下上限。**只记数，不预留、不封存**——封存是
 * emaster_session_observer_prepare_audit 的职责，它按运动时长算出容量后一次性 reserve
 * 再 seal。在它之前自行 seal 会让它的 reserve 撞上 capacity_sealed 而返回 false，
 * 那条 false 被 session_start 当作 AUDIT_FAILED，整轮会话在 OP 之前就转 FAULTED。
 * 返回是否真的设了上限；没设时后续行为与没有这个开关逐字一致。
 */
bool emaster_run_audit_apply_capacity_limit(emaster_run_audit_t *audit);
/*
 * 把算出来的容量压低到上限之内。上限为 0、或算出来的容量本就更小时原样返回。
 * 结果不会低于已记录条数：低于它会让 append_access 立刻判定容量已满。
 */
size_t emaster_run_audit_clamp_capacity(const emaster_run_audit_t *audit, size_t capacity);
/*
 * 仅在周期已停止后调用，允许后续 SDO 诊断继续保存完整记录。
 * 封存状态下不能走到停机诊断——那条路会置 allocation_failed，而它被当作
 * AUDIT_FAILED 上报并让会话转 FAULTED。
 * 解封**只放开"能写到哪"，不放开"能长到哪"**：停机阶段最多用到预留总量
 * （final_capacity），用尽后只计数。
 */
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
