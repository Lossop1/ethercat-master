#define _POSIX_C_SOURCE 200809L

#include "soem_common.h"
#include "session_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void emaster_soem_sdo_context_init(
    emaster_soem_sdo_reader_context_t *reader,
    ecx_contextt *context,
    uint16_t slave,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint64_t exchange)
{
    if (reader == NULL)
    {
        return;
    }
    reader->context = context;
    reader->slave = slave;
    reader->audit = audit;
    reader->phase = phase;
    reader->exchange = exchange;
    /* 默认沿用 SOEM 的全长邮箱超时；需要收紧的调用者用 set_timeout。 */
    reader->timeout_us = EC_TIMEOUTRXM;
}

void emaster_soem_sdo_set_timeout(emaster_soem_sdo_reader_context_t *reader, int us)
{
    if (reader == NULL)
    {
        return;
    }
    /*
     * 只允许收紧，不允许放宽：放宽的后果是一个不可达的从站能把调用方卡到
     * 比 SOEM 默认更久，那不是任何调用者想要的。非法值一律退回默认。
     */
    reader->timeout_us = (us > 0 && us <= EC_TIMEOUTRXM) ? us : EC_TIMEOUTRXM;
}

static void record_sdo(emaster_soem_sdo_reader_context_t *reader,
                       emaster_audit_direction_t direction,
                       emaster_audit_value_kind_t value_kind,
                       uint16_t index, uint8_t subindex, const void *raw,
                       uint8_t size, bool succeeded, uint64_t unsigned_value,
                       int64_t signed_value)
{
    uint8_t recorded_size =
        direction == EMASTER_AUDIT_DIRECTION_WRITE || succeeded ? size : 0U;

    if (reader != NULL && reader->audit != NULL)
    {
        (void)emaster_run_audit_record_access(
            reader->audit, reader->phase, EMASTER_AUDIT_TRANSPORT_SDO, direction,
            value_kind, reader->slave, index, subindex, (uint8_t)(size * UINT8_C(8)),
            0U, reader->exchange, raw, recorded_size, succeeded, unsigned_value,
            signed_value);
    }
}

static bool write_integer(emaster_soem_sdo_reader_context_t *reader,
                          uint16_t index, uint8_t subindex, const void *raw,
                          uint8_t size, emaster_audit_value_kind_t value_kind,
                          uint64_t unsigned_value, int64_t signed_value)
{
    bool succeeded = reader != NULL && reader->context != NULL && raw != NULL &&
                     ecx_SDOwrite(reader->context, reader->slave, index, subindex,
                                  FALSE, (int)size, raw, reader->timeout_us) > 0;

    record_sdo(reader, EMASTER_AUDIT_DIRECTION_WRITE, value_kind, index, subindex,
               raw, size, succeeded, unsigned_value, signed_value);
    return succeeded;
}

bool emaster_soem_write_u8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, uint8_t value)
{
    return write_integer(reader, index, subindex, &value, (uint8_t)sizeof(value),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_u16(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint16_t value)
{
    uint16_t raw = htoes(value);

    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_u32(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint32_t value)
{
    uint32_t raw = htoel(value);

    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_i8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, int8_t value)
{
    return write_integer(reader, index, subindex, &value, (uint8_t)sizeof(value),
                         EMASTER_AUDIT_VALUE_SIGNED, (uint8_t)value, value);
}

bool emaster_soem_write_i16(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, int16_t value)
{
    uint16_t host_value;
    uint16_t raw;

    memcpy(&host_value, &value, sizeof(host_value));
    raw = htoes(host_value);
    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_SIGNED, host_value, value);
}

bool emaster_soem_write_i32(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, int32_t value)
{
    uint32_t host_value;
    uint32_t raw;

    memcpy(&host_value, &value, sizeof(host_value));
    raw = htoel(host_value);
    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_SIGNED, host_value, value);
}

static bool assignment_failed(emaster_soem_sdo_reader_context_t *reader,
                               emaster_soem_pdo_assignment_result_t *result,
                               uint16_t index, uint8_t subindex)
{
    ec_errort error;

    /*
     * 七个调用点都在已经判过 result 非空的函数里，这一句永远不成立。留着它是因为
     * cppcheck 不跨函数推：它只看得到本函数无条件解引用空指针可能（nullPointer-
     * RedundantCheck / ctunullpointer），而 CI 的静态分析带 --error-exitcode=1。
     * 加一句判空比加一条抑制更省事，也不设"以后可以关掉分析器"的先例。
     */
    if (result == NULL)
    {
        return false;
    }
    result->failed_index = index;
    result->failed_subindex = subindex;
    /*
     * P9.2 更正：这里直接调 ecx_poperror（不取 session->error_ring_mutex），是**故意**
     * 的，不是漏加的锁——本函数只在 PDO 分配阶段被调用，那时会话还在 PreOP、观测线程
     * 尚未启动，环的写入方只有本线程。P4.5 原注释说"需要通过 reader->user_data 传递
     * 完整的 session"，但 emaster_soem_sdo_reader_context_t 里从来没有 user_data 字段，
     * 那句话描述的是一个不存在的改法。
     *
     * 循环把整环排空、只挑出与失败点匹配的那条：不匹配的多半是同一次 PreOP 序列里
     * 别的操作留下的，它们同样进了审计记录，所以这里丢弃不等于信息消失。运行期的环
     * 由 emaster_soem_drain_errors 取（周期每拍一次），不会走到这个函数。
     */
    while (reader != NULL && reader->context != NULL &&
           ecx_poperror(reader->context, &error))
    {
        if (error.Etype == EC_ERR_TYPE_SDO_ERROR && error.Index == index &&
            error.SubIdx == subindex)
        {
            result->abort_code_available = true;
            result->abort_code = (uint32_t)error.AbortCode;
        }
    }
    return false;
}

/* SDO 中止码：子索引不存在。 */
#define EMASTER_SDO_ABORT_NO_SUBINDEX UINT32_C(0x06090011)

/* 写失败之后把中止码取出来看清楚，返回值沿用 assignment_failed 的失败语义。 */
static bool abort_code_is(emaster_soem_sdo_reader_context_t *reader,
                          emaster_soem_pdo_assignment_result_t *result,
                          uint16_t index, uint8_t subindex, uint32_t code)
{
    (void)assignment_failed(reader, result, index, subindex);
    return result->abort_code_available && result->abort_code == code;
}

/*
 * 断开分配：条目数写 0 并读回。
 *
 * 本来就是 0 就不写这一句。多写一次没好处，而且在"按条目数决定子索引可见性"的固件上
 * 反而有害：断开之后 1C12:01 就不存在了。
 *
 * touched 只在这里真的写过之后才置真，供还原逻辑判断这个对象要不要写回去。
 */
static bool detach_direction(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                             bool *touched, emaster_soem_pdo_assignment_result_t *result)
{
    uint8_t count;

    if (touched != NULL)
    {
        *touched = false;
    }
    if (!emaster_soem_read_u8(reader, index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    if (count == 0U)
    {
        return true;
    }
    if (!emaster_soem_write_u8(reader, index, UINT8_C(0), UINT8_C(0)) ||
        !emaster_soem_read_u8(reader, index, UINT8_C(0), &count) || count != 0U)
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    if (touched != NULL)
    {
        *touched = true;
    }
    return true;
}

/*
 * 把本方向的映射表挂到分配对象上。顺序照厂商手册的三步：断开 -> 写通道号 -> 写激活数。
 *
 * 一处与"边写边读回"的直觉写法不同：**条目号写完之前不读回**。有的固件按条目数决定
 * 子索引可不可见（MT 台架实测）：条目数为 0 时，写 1C12:01 设备收下，随即读 1C12:01
 * 却回中止码 0x06090011（子索引不存在）。逐条写、逐条读回就会在这里误判成"写失败"而
 * 掉头，其实是自己把顺序读早了。读回统一放到"写激活数"之后。
 *
 * 若某台设备连写都拒（同样回 0x06090011），就把激活数先写上去让子索引出现再写一次；
 * PRE-OP 下同步管理器没运行，中间那个半挂的分配不会被用上。认的是设备自己的答复，
 * 不是"哪个厂商的设备"。
 */
static bool attach_direction(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                             const emaster_pdo_mapping_profile_t *mappings,
                             size_t mapping_count,
                             emaster_soem_pdo_assignment_result_t *result)
{
    size_t ordinal;
    uint8_t count;
    bool raised_count = false;

    if (!detach_direction(reader, index, NULL, result))
    {
        return false;
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        uint8_t subindex = (uint8_t)(ordinal + 1U);

        if (!emaster_soem_write_u16(reader, index, subindex, mappings[ordinal].index))
        {
            if (raised_count ||
                !abort_code_is(reader, result, index, subindex,
                               EMASTER_SDO_ABORT_NO_SUBINDEX))
            {
                return false;
            }
            if (!emaster_soem_write_u8(reader, index, UINT8_C(0), (uint8_t)mapping_count))
            {
                return assignment_failed(reader, result, index, UINT8_C(0));
            }
            raised_count = true;
            if (!emaster_soem_write_u16(reader, index, subindex, mappings[ordinal].index))
            {
                return assignment_failed(reader, result, index, subindex);
            }
        }
    }
    if (!emaster_soem_write_u8(reader, index, UINT8_C(0), (uint8_t)mapping_count) ||
        !emaster_soem_read_u8(reader, index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    if (count != (uint8_t)mapping_count)
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        uint16_t observed_index;

        if (!emaster_soem_read_u16(reader, index, (uint8_t)(ordinal + 1U),
                                   &observed_index) ||
            observed_index != mappings[ordinal].index)
        {
            return assignment_failed(reader, result, index, (uint8_t)(ordinal + 1U));
        }
    }
    return true;
}

static bool assignment_matches(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                               const emaster_pdo_mapping_profile_t *mappings,
                               size_t mapping_count, bool *matches,
                               emaster_soem_pdo_assignment_result_t *result)
{
    uint8_t count;
    size_t ordinal;

    if (mappings == NULL || matches == NULL || result == NULL ||
        !emaster_soem_read_u8(reader, index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    *matches = (size_t)count == mapping_count;
    for (ordinal = 0U; *matches && ordinal < mapping_count; ++ordinal)
    {
        uint16_t observed_index;

        if (!emaster_soem_read_u16(reader, index, (uint8_t)(ordinal + 1U),
                                   &observed_index))
        {
            return assignment_failed(reader, result, index,
                                     (uint8_t)(ordinal + 1U));
        }
        *matches = observed_index == mappings[ordinal].index;
    }
    return true;
}

/*
 * F030 是模块化从站的配置模块列表。非模块化设备（直连档）没有这个对象，写它只会换来
 * SDO 中止，所以整段跳过。
 */
static bool select_module_if_needed(emaster_soem_sdo_reader_context_t *reader,
                                    const emaster_pdo_set_profile_t *pdo_set,
                                    emaster_soem_pdo_assignment_result_t *result)
{
    uint32_t module_ident;
    uint32_t observed_module_ident;

    if (pdo_set->assignment_style != EMASTER_PDO_ASSIGNMENT_MODULE_SLOT)
    {
        return true;
    }
    module_ident = pdo_set->module_ident;
    if (!emaster_soem_write_u32(reader, UINT16_C(0xF030), pdo_set->module_slot,
                                module_ident) ||
        !emaster_soem_read_u32(reader, UINT16_C(0xF030), pdo_set->module_slot,
                               &observed_module_ident) ||
        observed_module_ident != module_ident)
    {
        return assignment_failed(reader, result, UINT16_C(0xF030), pdo_set->module_slot);
    }
    return true;
}

/*
 * 改写是"先断开分配、再改映射表、最后挂回分配"。断开那一步一旦成功，设备就不是动手前
 * 的样子了；后面任何一步失败——设备拒写映射表（MT 台架实测 0x1600:00 回 0x06010002）、
 * 读回不符、掉链——就地返回会把设备留在"分配被清零"的半途：PDO 不再交换，这次上电期间
 * 都用不了。所以动手前先记现场，失败就还原。
 */
enum
{
    ROLLBACK_MAX_MAPPINGS = 4,
    ROLLBACK_MAX_ENTRIES = 16
};

/*
 * dirty 只由"动手的那一句写成功了"来置位。没动过的对象不还原 —— 还原就是往里写一遍，
 * 那对只读对象同样会被拒，而拒一次就足以让整段还原半途而废，把分配留在 0。
 */
typedef struct
{
    uint16_t index;
    uint8_t count;
    bool dirty;
    uint16_t entries[ROLLBACK_MAX_ENTRIES];
} assignment_snapshot_t;

typedef struct
{
    uint16_t index;
    uint8_t count;
    bool dirty;
    uint32_t descriptors[ROLLBACK_MAX_ENTRIES];
} mapping_snapshot_t;

typedef struct
{
    assignment_snapshot_t assignment;
    mapping_snapshot_t tables[ROLLBACK_MAX_MAPPINGS];
    size_t table_count;
    bool complete;
} direction_snapshot_t;

static bool capture_assignment(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                               assignment_snapshot_t *snapshot)
{
    uint8_t ordinal;

    snapshot->index = index;
    if (!emaster_soem_read_u8(reader, index, UINT8_C(0), &snapshot->count) ||
        snapshot->count > ROLLBACK_MAX_ENTRIES)
    {
        return false;
    }
    for (ordinal = 0U; ordinal < snapshot->count; ++ordinal)
    {
        if (!emaster_soem_read_u16(reader, index, (uint8_t)(ordinal + 1U),
                                   &snapshot->entries[ordinal]))
        {
            return false;
        }
    }
    return true;
}

static bool capture_mapping(emaster_soem_sdo_reader_context_t *reader,
                            const emaster_pdo_mapping_profile_t *mapping,
                            mapping_snapshot_t *snapshot)
{
    uint8_t ordinal;

    snapshot->index = mapping->index;
    if (!emaster_soem_read_u8(reader, mapping->index, UINT8_C(0), &snapshot->count) ||
        snapshot->count > ROLLBACK_MAX_ENTRIES)
    {
        return false;
    }
    for (ordinal = 0U; ordinal < snapshot->count; ++ordinal)
    {
        if (!emaster_soem_read_u32(reader, mapping->index, (uint8_t)(ordinal + 1U),
                                   &snapshot->descriptors[ordinal]))
        {
            return false;
        }
    }
    return true;
}

/* 逐条写回并读回校验，任一步不成立就停手。返回值只说明这次还原动作有没有走完。 */
static bool restore_assignment(emaster_soem_sdo_reader_context_t *reader,
                               const assignment_snapshot_t *snapshot)
{
    uint8_t count;
    uint8_t ordinal;

    if (!emaster_soem_write_u8(reader, snapshot->index, UINT8_C(0), UINT8_C(0)) ||
        !emaster_soem_read_u8(reader, snapshot->index, UINT8_C(0), &count) || count != 0U)
    {
        return false;
    }
    for (ordinal = 0U; ordinal < snapshot->count; ++ordinal)
    {
        uint16_t observed;

        if (!emaster_soem_write_u16(reader, snapshot->index, (uint8_t)(ordinal + 1U),
                                    snapshot->entries[ordinal]) ||
            !emaster_soem_read_u16(reader, snapshot->index, (uint8_t)(ordinal + 1U),
                                   &observed) ||
            observed != snapshot->entries[ordinal])
        {
            return false;
        }
    }
    return emaster_soem_write_u8(reader, snapshot->index, UINT8_C(0), snapshot->count);
}

static bool restore_mapping(emaster_soem_sdo_reader_context_t *reader,
                            const mapping_snapshot_t *snapshot)
{
    uint8_t count;
    uint8_t ordinal;

    if (!emaster_soem_write_u8(reader, snapshot->index, UINT8_C(0), UINT8_C(0)) ||
        !emaster_soem_read_u8(reader, snapshot->index, UINT8_C(0), &count) || count != 0U)
    {
        return false;
    }
    for (ordinal = 0U; ordinal < snapshot->count; ++ordinal)
    {
        uint32_t observed;

        if (!emaster_soem_write_u32(reader, snapshot->index, (uint8_t)(ordinal + 1U),
                                    snapshot->descriptors[ordinal]) ||
            !emaster_soem_read_u32(reader, snapshot->index, (uint8_t)(ordinal + 1U),
                                   &observed) ||
            observed != snapshot->descriptors[ordinal])
        {
            return false;
        }
    }
    return emaster_soem_write_u8(reader, snapshot->index, UINT8_C(0), snapshot->count);
}

static bool capture_direction(emaster_soem_sdo_reader_context_t *reader,
                              uint16_t assignment_index,
                              const emaster_pdo_mapping_profile_t *mappings,
                              size_t mapping_count, direction_snapshot_t *snapshot,
                              emaster_soem_pdo_assignment_result_t *result)
{
    size_t ordinal;

    memset(snapshot, 0, sizeof(*snapshot));
    if (mapping_count > ROLLBACK_MAX_MAPPINGS)
    {
        /* 装不下就不动手：没有现场可回滚的改写，不如整段不做。 */
        return assignment_failed(reader, result, assignment_index, UINT8_C(0));
    }
    snapshot->table_count = mapping_count;
    if (!capture_assignment(reader, assignment_index, &snapshot->assignment))
    {
        return assignment_failed(reader, result, assignment_index, UINT8_C(0));
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        if (!capture_mapping(reader, &mappings[ordinal], &snapshot->tables[ordinal]))
        {
            return assignment_failed(reader, result, mappings[ordinal].index, UINT8_C(0));
        }
    }
    snapshot->complete = true;
    return true;
}

/*
 * 还原顺序与改写顺序相反：先把动过的映射表写回原样，再挂回分配。任何一张动过的表没还原
 * 成功就不再挂回分配——把分配指回一张半截的表，比让分配停在 0 更糟：前者主站会按错误的
 * 映像跑起来，后者设备只是停在 PRE-OP、不交换 PDO。
 */
static void rollback_direction(emaster_soem_sdo_reader_context_t *reader,
                               const direction_snapshot_t *snapshot)
{
    size_t ordinal;

    if (!snapshot->complete)
    {
        return;
    }
    for (ordinal = snapshot->table_count; ordinal > 0U; --ordinal)
    {
        const mapping_snapshot_t *table = &snapshot->tables[ordinal - 1U];

        if (table->dirty && !restore_mapping(reader, table))
        {
            return;
        }
    }
    if (snapshot->assignment.dirty)
    {
        (void)restore_assignment(reader, &snapshot->assignment);
    }
}

/*
 * 把一套映射表的**条目**按设备档重写一遍。
 *
 * 为什么需要它。1C12/1C13 只选"用哪几张映射表"，选对了不等于表里的内容对：
 * 设备出厂默认的表与厂商 ESI 声明的表可以逐条不同（MT 台架实测：ESI 说第 7 条是
 * 假字节 0x2FFD，设备实际映射 0x5FFE）。这时只有把条目本身写下去才会一致，
 * 而这正是 CoE 的 PdoConfig 能力；设备声明了它才允许走这条路。
 *
 * 顺序按标准：先把映射表的条目数写 0（表在编辑中不能被使用），逐条写描述符并读回，
 * 最后把条目数写回去。分配对象由 configure_direction 在更外层断开与重挂。
 */
static bool configure_mapping_entries(emaster_soem_sdo_reader_context_t *reader,
                                      const emaster_pdo_mapping_profile_t *mapping,
                                      mapping_snapshot_t *snapshot,
                                      emaster_soem_pdo_assignment_result_t *result)
{
    uint8_t count;
    size_t ordinal;

    if (mapping->entry_count > UINT8_MAX ||
        (mapping->entry_count > 0U && mapping->entries == NULL))
    {
        return assignment_failed(reader, result, mapping->index, UINT8_C(0));
    }
    if (!emaster_soem_write_u8(reader, mapping->index, UINT8_C(0), UINT8_C(0)) ||
        !emaster_soem_read_u8(reader, mapping->index, UINT8_C(0), &count) || count != 0U)
    {
        return assignment_failed(reader, result, mapping->index, UINT8_C(0));
    }
    /* 从这一句起表已经被动过；此后失败就必须还原。拒在上一句的设备（MT 就是）
     * 表还是出厂原样，这时还原反而是往里再写一遍、再被拒一次。 */
    snapshot->dirty = true;
    for (ordinal = 0U; ordinal < mapping->entry_count; ++ordinal)
    {
        const emaster_pdo_entry_t *entry = &mapping->entries[ordinal];
        uint8_t subindex = (uint8_t)(ordinal + 1U);
        uint32_t descriptor = emaster_pdo_mapping_descriptor(
            entry->index, entry->subindex, entry->bit_length);
        uint32_t observed_descriptor;

        if (!emaster_soem_write_u32(reader, mapping->index, subindex, descriptor) ||
            !emaster_soem_read_u32(reader, mapping->index, subindex, &observed_descriptor) ||
            observed_descriptor != descriptor)
        {
            return assignment_failed(reader, result, mapping->index, subindex);
        }
    }
    if (!emaster_soem_write_u8(reader, mapping->index, UINT8_C(0),
                               (uint8_t)mapping->entry_count) ||
        !emaster_soem_read_u8(reader, mapping->index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, mapping->index, UINT8_C(0));
    }
    return count == (uint8_t)mapping->entry_count;
}

/*
 * 一个方向的写序列：断开分配 -> 逐张映射表重写条目 -> 重挂分配。
 *
 * 必须按这个顺序：分配还没断开时改映射表，多半换来 SDO 中止 —— 表正在被同步管理器使用。
 */
static bool write_direction(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t assignment_index,
                            const emaster_pdo_mapping_profile_t *mappings,
                            size_t mapping_count, direction_snapshot_t *snapshot,
                            emaster_soem_pdo_assignment_result_t *result)
{
    size_t ordinal;

    if (!detach_direction(reader, assignment_index, &snapshot->assignment.dirty, result))
    {
        return false;
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        if (!configure_mapping_entries(reader, &mappings[ordinal], &snapshot->tables[ordinal],
                                      result))
        {
            return false;
        }
    }
    return attach_direction(reader, assignment_index, mappings, mapping_count, result);
}

/*
 * 一个方向（Rx 或 Tx）的完整配置序列：记现场 -> 写 -> 失败还原。写成功时现场自然作废。
 */
static bool configure_direction(emaster_soem_sdo_reader_context_t *reader,
                                uint16_t assignment_index,
                                const emaster_pdo_mapping_profile_t *mappings,
                                size_t mapping_count,
                                emaster_soem_pdo_assignment_result_t *result)
{
    direction_snapshot_t snapshot;

    if (mapping_count > UINT8_MAX || (mapping_count > 0U && mappings == NULL))
    {
        return assignment_failed(reader, result, assignment_index, UINT8_C(0));
    }
    if (!capture_direction(reader, assignment_index, mappings, mapping_count, &snapshot,
                           result))
    {
        return false;
    }
    if (write_direction(reader, assignment_index, mappings, mapping_count, &snapshot, result))
    {
        return true;
    }
    rollback_direction(reader, &snapshot);
    return false;
}

/*
 * 只改分配、不动映射表内容的那条路。同样先记现场再动手：断开之后合不回去的分配，
 * 比一开始就不动它更糟——这一路上台架实测过（1C12:00 停在 0，设备此后不映射 PDO）。
 */
static bool assign_direction(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                             const emaster_pdo_mapping_profile_t *mappings,
                             size_t mapping_count,
                             emaster_soem_pdo_assignment_result_t *result)
{
    assignment_snapshot_t snapshot;

    if (mapping_count > UINT8_MAX || (mapping_count > 0U && mappings == NULL))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (!capture_assignment(reader, index, &snapshot))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    if (attach_direction(reader, index, mappings, mapping_count, result))
    {
        return true;
    }
    (void)restore_assignment(reader, &snapshot);
    return false;
}

bool emaster_soem_configure_pdo_set_recorded(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_pdo_set_profile_t *pdo_set,
    emaster_soem_pdo_assignment_result_t *result)
{
    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
    }
    if (reader == NULL || reader->context == NULL || reader->slave == 0U ||
        pdo_set == NULL || result == NULL || pdo_set->rx_mappings == NULL ||
        pdo_set->tx_mappings == NULL)
    {
        return false;
    }
    if (!select_module_if_needed(reader, pdo_set, result))
    {
        return false;
    }
    return configure_direction(reader, UINT16_C(0x1C12), pdo_set->rx_mappings,
                               pdo_set->rx_mapping_count, result) &&
           configure_direction(reader, UINT16_C(0x1C13), pdo_set->tx_mappings,
                               pdo_set->tx_mapping_count, result);
}

bool emaster_soem_assign_pdo_set_recorded(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_pdo_set_profile_t *pdo_set,
    emaster_soem_pdo_assignment_result_t *result)
{
    bool rx_matches;
    bool tx_matches;

    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
    }
    if (reader == NULL || reader->context == NULL || reader->slave == 0U ||
        pdo_set == NULL || result == NULL ||
        pdo_set->rx_mappings == NULL || pdo_set->tx_mappings == NULL)
    {
        return false;
    }
    if (!select_module_if_needed(reader, pdo_set, result))
    {
        return false;
    }
    if (!assignment_matches(reader, UINT16_C(0x1C12), pdo_set->rx_mappings,
                            pdo_set->rx_mapping_count, &rx_matches, result) ||
        !assignment_matches(reader, UINT16_C(0x1C13), pdo_set->tx_mappings,
                            pdo_set->tx_mapping_count, &tx_matches, result))
    {
        return false;
    }
    return (rx_matches ||
            assign_direction(reader, UINT16_C(0x1C12), pdo_set->rx_mappings,
                             pdo_set->rx_mapping_count, result)) &&
           (tx_matches ||
            assign_direction(reader, UINT16_C(0x1C13), pdo_set->tx_mappings,
                             pdo_set->tx_mapping_count, result));
}

bool emaster_soem_assign_pdo_set(ecx_contextt *context, uint16_t slave,
                                 const emaster_pdo_set_profile_t *pdo_set,
                                 emaster_soem_pdo_assignment_result_t *result)
{
    emaster_soem_sdo_reader_context_t reader;

    emaster_soem_sdo_context_init(&reader, context, slave, NULL,
                                  EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, 0U);
    return emaster_soem_assign_pdo_set_recorded(&reader, pdo_set, result);
}

bool emaster_soem_restore_init(ecx_contextt *context)
{
    if (context == NULL)
    {
        return false;
    }
    context->slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(context, 0U);
    return ecx_statecheck(context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE) == EC_STATE_INIT;
}

bool emaster_soem_interface_carrier(const char *interface_name)
{
    char path[256];
    char carrier[4];
    FILE *stream;

    if (interface_name == NULL || interface_name[0] == '\0' ||
        strnlen(interface_name, sizeof(path)) >= sizeof(path) - 24U)
    {
        return false;
    }
    if (snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", interface_name) < 0)
    {
        return false;
    }
    stream = fopen(path, "r");
    if (stream == NULL)
    {
        return false;
    }
    memset(carrier, 0, sizeof(carrier));
    if (fgets(carrier, sizeof(carrier), stream) == NULL)
    {
        (void)fclose(stream);
        return false;
    }
    (void)fclose(stream);
    return carrier[0] == '1';
}

bool emaster_soem_wait_preop(ecx_contextt *context)
{
    if (context == NULL)
    {
        return false;
    }
    return ecx_statecheck(context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4) ==
           EC_STATE_PRE_OP;
}

static bool read_integer(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                         uint8_t subindex, void *value, int size)
{
    int actual_size = size;

    if (reader == NULL || reader->context == NULL || value == NULL || size <= 0)
    {
        return false;
    }
    return ecx_SDOread(reader->context, reader->slave, index, subindex, FALSE, &actual_size,
                       value, reader->timeout_us) > 0 &&
           actual_size == size;
}

bool emaster_soem_read_u8(void *user_data, uint16_t index, uint8_t subindex,
                          uint8_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint8_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    if (succeeded)
    {
        *value = raw;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, raw, (int64_t)raw);
    return succeeded;
}

bool emaster_soem_read_u16(void *user_data, uint16_t index, uint8_t subindex,
                           uint16_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));
    uint16_t decoded = etohs(raw);

    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, decoded,
               (int64_t)decoded);
    return succeeded;
}

bool emaster_soem_read_u32(void *user_data, uint16_t index, uint8_t subindex,
                           uint32_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));
    uint32_t decoded = etohl(raw);

    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, decoded,
               (int64_t)decoded);
    return succeeded;
}

bool emaster_soem_read_i8(void *user_data, uint16_t index, uint8_t subindex,
                          int8_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    int8_t raw = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    if (succeeded)
    {
        *value = raw;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, (uint8_t)raw,
               raw);
    return succeeded;
}

bool emaster_soem_read_i16(void *user_data, uint16_t index, uint8_t subindex,
                           int16_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw = 0U;
    uint16_t host_value;
    int16_t decoded = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    host_value = etohs(raw);
    memcpy(&decoded, &host_value, sizeof(decoded));
    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, host_value,
               decoded);
    return succeeded;
}

bool emaster_soem_read_i32(void *user_data, uint16_t index, uint8_t subindex,
                           int32_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw = 0U;
    uint32_t host_value;
    int32_t decoded = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    host_value = etohl(raw);
    memcpy(&decoded, &host_value, sizeof(decoded));
    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, host_value,
               decoded);
    return succeeded;
}

bool emaster_soem_discover_pdo_layout_recorded(
    emaster_soem_sdo_reader_context_t *reader_context,
    emaster_pdo_layout_t *layout)
{
    emaster_pdo_sdo_reader_t reader;

    if (reader_context == NULL || reader_context->context == NULL ||
        reader_context->slave == 0U || layout == NULL)
    {
        return false;
    }
    reader.read_u8 = emaster_soem_read_u8;
    reader.read_u16 = emaster_soem_read_u16;
    reader.read_u32 = emaster_soem_read_u32;
    reader.user_data = reader_context;
    return emaster_pdo_layout_discover(&reader, layout) == EMASTER_PDO_DISCOVERY_COMPLETE;
}

bool emaster_soem_discover_pdo_layout(ecx_contextt *context, uint16_t slave,
                                      emaster_pdo_layout_t *layout)
{
    emaster_soem_sdo_reader_context_t reader_context;

    emaster_soem_sdo_context_init(&reader_context, context, slave, NULL,
                                  EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, 0U);
    return emaster_soem_discover_pdo_layout_recorded(&reader_context, layout);
}

/* P4.5: 线程安全的错误环访问包装 */
bool emaster_soem_pop_error_safe(emaster_soem_session_t *session, ec_errort *error)
{
    bool result;

    if (session == NULL || error == NULL)
    {
        return false;
    }
    if (pthread_mutex_lock(&session->error_ring_mutex) != 0)
    {
        return false;
    }
    result = ecx_poperror(&session->context, error);
    (void)pthread_mutex_unlock(&session->error_ring_mutex);
    return result;
}

/* P9.2: 把一条取出的事件抄进报告的定长环。环满只计数——报告结构是定长的，
 * 丢掉了哪几条看不见，但"丢掉了几条"必须看得见。 */
static void record_error_event(emaster_soem_session_t *session, const ec_errort *error)
{
    emaster_control_session_report_t *report = session->report;
    emaster_soem_error_event_t *event;

    if (report == NULL)
    {
        return;
    }
    if (report->soem_error_count != UINT64_MAX)
    {
        ++report->soem_error_count;
    }
    if (report->soem_error_event_count >= EMASTER_SOEM_ERROR_CAPACITY)
    {
        if (report->soem_error_dropped_count != UINT64_MAX)
        {
            ++report->soem_error_dropped_count;
        }
        return;
    }
    event = &report->soem_errors[report->soem_error_event_count];
    memset(event, 0, sizeof(*event));
    /* 取出时的交换号：事件本身不知道自己发生在哪一拍，只能记"什么时候被看见"。
     * 周期里每拍都取，所以这个差值不超过一拍。 */
    event->exchange = session->exchange;
    if (error->Time.tv_sec >= 0 && error->Time.tv_nsec >= 0)
    {
        event->time_unix_ns = (uint64_t)error->Time.tv_sec * UINT64_C(1000000000) +
                              (uint64_t)error->Time.tv_nsec;
    }
    event->slave = error->Slave;
    event->index = error->Index;
    event->subindex = error->SubIdx;
    event->etype = (uint8_t)error->Etype;
    event->error_code = error->ErrorCode;
    /*
     * AbortCode 与 ErrorCode 在 SOEM 里是同一个联合体成员位置，靠 Etype 区分语义：
     * SDO/SDOinfo 填 AbortCode，SoE 和紧急报文填 ErrorCode（见 ec_coe.c:65、ec_soe.c:46、
     * ec_main.c:151/172）。取错字段会得到一个看着像错误码的垃圾值，所以这里显式分开。
     */
    if (error->Etype == EC_ERR_TYPE_SDO_ERROR || error->Etype == EC_ERR_TYPE_SDOINFO_ERROR)
    {
        event->abort_code = error->AbortCode;
        event->abort_code_valid = true;
    }
    ++report->soem_error_event_count;
}

/*
 * P9.2: 取用点。周期回路每拍调一次，停机收尾再调一次。
 *
 * 空环快路径先不加锁读一次头尾：并发的 ecx_pusherror 只会让这里读到"暂时为空"，
 * 那条事件下一拍再取——错误环是记账通道，晚一拍不影响任何判断，而每一拍都去抢
 * 一次互斥锁，是拿周期线程的时间去换一条诊断记录的及时性，不划算。
 * 真正取的时候仍然走 emaster_soem_pop_error_safe 的互斥保护。
 */
void emaster_soem_drain_errors(emaster_soem_session_t *session)
{
    ec_errort error;

    if (session == NULL || session->report == NULL)
    {
        return;
    }
    if (session->context.elist.head == session->context.elist.tail)
    {
        return;
    }
    while (emaster_soem_pop_error_safe(session, &error))
    {
        record_error_event(session, &error);
    }
}
