#include "emaster/audit/run_audit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static emaster_audit_access_t *append_access(emaster_run_audit_t *audit)
{
    emaster_audit_access_t *resized;
    size_t new_capacity;

    if (audit == NULL || audit->allocation_failed)
    {
        return NULL;
    }
    if (audit->access_count == audit->access_capacity)
    {
        /*
         * 硬上限：预留之后（final_capacity 非 0）无论封不封存都不再扩数组。
         * 正常路径到不了这里——两个 record 函数都会先按各自的额度判定并计数；
         * 这条是兜底，宁可不记也不在周期线程里分配。
         */
        if (audit->final_capacity != 0U &&
            audit->access_capacity >= audit->final_capacity)
        {
            return NULL;
        }
        if (audit->capacity_sealed)
        {
            audit->allocation_failed = true;
            return NULL;
        }
        new_capacity = audit->access_capacity == 0U ? 64U : audit->access_capacity * 2U;
        if (new_capacity < audit->access_capacity ||
            new_capacity > SIZE_MAX / sizeof(*audit->accesses))
        {
            audit->allocation_failed = true;
            return NULL;
        }
        resized = realloc(audit->accesses, new_capacity * sizeof(*audit->accesses));
        if (resized == NULL)
        {
            audit->allocation_failed = true;
            return NULL;
        }
        audit->accesses = resized;
        audit->access_capacity = new_capacity;
    }
    memset(&audit->accesses[audit->access_count], 0,
           sizeof(audit->accesses[audit->access_count]));
    return &audit->accesses[audit->access_count++];
}

bool emaster_run_audit_reserve(emaster_run_audit_t *audit, size_t capacity)
{
    emaster_audit_access_t *resized;

    if (audit == NULL || audit->capacity_sealed || audit->allocation_failed)
    {
        return false;
    }
    if (capacity <= audit->access_capacity)
    {
        return true;
    }
    if (capacity > SIZE_MAX / sizeof(*audit->accesses))
    {
        audit->allocation_failed = true;
        return false;
    }
    resized = realloc(audit->accesses, capacity * sizeof(*audit->accesses));
    if (resized == NULL)
    {
        audit->allocation_failed = true;
        return false;
    }
    /* 预触及新增页，避免周期开始后首次写入大段审计内存时才分配物理页。 */
    memset(resized + audit->access_capacity, 0,
           (capacity - audit->access_capacity) * sizeof(*resized));
    audit->accesses = resized;
    audit->access_capacity = capacity;
    /* 预留一次定终身：此后 append_access 走到容量边界也不许再长。 */
    audit->final_capacity = capacity;
    return true;
}

void emaster_run_audit_seal_cyclic(emaster_run_audit_t *audit, size_t cyclic_capacity)
{
    if (audit != NULL)
    {
        audit->capacity_sealed = true;
        if (cyclic_capacity > audit->access_capacity)
        {
            cyclic_capacity = audit->access_capacity;
        }
        audit->sealed_capacity = cyclic_capacity;
    }
}

void emaster_run_audit_init(emaster_run_audit_t *audit)
{
    if (audit != NULL)
    {
        memset(audit, 0, sizeof(*audit));
    }
}

/*
 * 为什么需要一条上限：周期 PDO 记录按"轴 × 变化的字段"逐拍追加，运动时位置与目标
 * 每拍都在变，上面对 record_pdo 的合并条件就再也命中不了，记录数于是随时长线性增长。
 * 1 kHz 四轴实测约 4 MB/s：60 秒 243 MB，8 小时上百 GB——长时运行会先把内存吃光，
 * 再让报告写不出来。
 *
 * 而报告里真正用来判"这一轮行不行"的汇聚指标（帧距、截止时间、WKC、发布耗时）都不
 * 依赖这条逐拍轨迹。所以长时运行应当给它一个上限，满了只计数（omitted_pdo_samples，
 * 报告与控制台都会标出来），而不是让整轮实验做不成。
 *
 * 默认不设：环境变量缺席时返回 false，容量仍按需倍增——与加这个开关之前逐字一致。
 */
bool emaster_run_audit_apply_capacity_limit(emaster_run_audit_t *audit)
{
    const char *text;
    char *end = NULL;
    unsigned long long limit;

    if (audit == NULL)
    {
        return false;
    }
    text = getenv("EMASTER_AUDIT_MAX_ACCESSES");
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }
    errno = 0;
    limit = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || limit == 0ULL)
    {
        fprintf(stderr, "EMASTER_AUDIT_MAX_ACCESSES 不是正整数（%s），本次不设上限\n", text);
        fflush(stderr);
        return false;
    }
    if (limit > (unsigned long long)SIZE_MAX)
    {
        limit = (unsigned long long)SIZE_MAX;
    }
    audit->capacity_limit = (size_t)limit;
    return true;
}

size_t emaster_run_audit_clamp_capacity(const emaster_run_audit_t *audit, size_t capacity)
{
    if (audit == NULL)
    {
        return capacity;
    }
    if (audit->capacity_limit != 0U && capacity > audit->capacity_limit)
    {
        capacity = audit->capacity_limit;
    }
    /* 下限是已记录条数：低于它，下一次 append_access 会立刻判定容量已满并置
     * allocation_failed，等于把上限开关变成一次自伤。 */
    if (capacity < audit->access_count)
    {
        capacity = audit->access_count;
    }
    return capacity;
}

void emaster_run_audit_end_cyclic(emaster_run_audit_t *audit)
{
    if (audit != NULL)
    {
        audit->capacity_sealed = false;
    }
}

void emaster_run_audit_destroy(emaster_run_audit_t *audit)
{
    if (audit == NULL)
    {
        return;
    }
    free(audit->accesses);
    memset(audit, 0, sizeof(*audit));
}

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
    int64_t signed_value)
{
    emaster_audit_access_t *access;

    if (audit == NULL || raw_size > sizeof(((emaster_audit_access_t *)0)->raw) ||
        (raw_size > 0U && raw == NULL))
    {
        return false;
    }
    /*
     * 容量判定放在这里，不放 append_access 里：函数返回 false 会被上游当成审计失败，
     * 所以"满了"只能表现为"这条不记、计数加一、返回成功"。
     * 额度分两段，计的两笔账也按段分——**按段而不是按阶段**：
     *   封存中撞上水线（周期段吃满）——不论记的是 PDO 还是别的一律进 omitted_pdo_samples，
     *     与 emaster_run_audit_record_pdo 同一笔账；
     *   预留之后撞上预留总量（停机段吃满）——进 omitted_final_samples。
     * 这样"哪一段被吃光"能从数上直接看出来。
     * **最后的 final_capacity != 0 是必需的**：预留之前两个计数都是 0，`0 >= 0` 成立，
     * 少了这个条件，会话建立阶段（PRE-OP 配置读写）的每一条记录都会被当成"额度已满"
     * 直接丢掉——审计里最该留的配置轨迹反而一条不剩。
     */
    if (audit->capacity_sealed)
    {
        if (audit->access_count >= audit->sealed_capacity)
        {
            if (audit->omitted_pdo_samples != UINT64_MAX)
            {
                ++audit->omitted_pdo_samples;
            }
            return true;
        }
    }
    else if (audit->final_capacity != 0U &&
             audit->access_count >= audit->access_capacity)
    {
        if (audit->omitted_final_samples != UINT64_MAX)
        {
            ++audit->omitted_final_samples;
        }
        return true;
    }
    access = append_access(audit);
    if (access == NULL)
    {
        return false;
    }
    access->order = audit->next_order++;
    access->sample_count = 1U;
    access->first_exchange = exchange;
    access->last_exchange = exchange;
    access->phase = phase;
    access->transport = transport;
    access->direction = direction;
    access->value_kind = value_kind;
    access->slave_position = slave_position;
    access->index = index;
    access->subindex = subindex;
    access->bit_length = bit_length;
    access->bit_offset = bit_offset;
    access->raw_size = raw_size;
    access->succeeded = succeeded;
    access->unsigned_value = unsigned_value;
    access->signed_value = signed_value;
    if (raw_size > 0U)
    {
        memcpy(access->raw, raw, raw_size);
    }
    return true;
}

static bool same_pdo_field(const emaster_audit_access_t *access,
                           emaster_audit_phase_t phase,
                           emaster_audit_direction_t direction,
                           emaster_audit_value_kind_t value_kind,
                           uint16_t slave_position,
                           uint16_t index,
                           uint8_t subindex,
                           uint8_t bit_length,
                           uint32_t bit_offset)
{
    return access->transport == EMASTER_AUDIT_TRANSPORT_PDO &&
           access->phase == phase && access->direction == direction &&
           access->value_kind == value_kind &&
           access->slave_position == slave_position && access->index == index &&
           access->subindex == subindex && access->bit_length == bit_length &&
           access->bit_offset == bit_offset;
}

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
    int64_t signed_value)
{
    emaster_audit_access_t *access;

    if (audit == NULL || last_record == NULL)
    {
        return false;
    }
    if (*last_record < audit->access_count)
    {
        access = &audit->accesses[*last_record];
        if (same_pdo_field(access, phase, direction, value_kind, slave_position,
                           index, subindex, bit_length, bit_offset))
        {
            if (access->unsigned_value == unsigned_value &&
                access->signed_value == signed_value &&
                access->succeeded == succeeded &&
                exchange > access->last_exchange &&
                exchange - access->last_exchange == UINT64_C(1))
            {
                access->last_exchange = exchange;
                ++access->sample_count;
                return true;
            }
        }
    }
    /*
     * 容量判定分两段，都不分配内存：
     *   封存中（周期阶段）——用到 sealed_capacity 为止，余下的留给停机诊断；
     *   已解封（停机阶段）——可以用到预留总量 final_capacity 为止。
     * 两段都满了就只计数。这条"只计数"的分支是 P8.5 的修复点：以前解封时数组
     * 正好是满的，停机后第一条记录就会 realloc 一次 84.6 MB 的块（内核 mremap
     * 实测 3.757 ms），落在周期尾部把下一帧推迟 4 ms，五轴全掉出 OP。
     * final_capacity != 0 与 record_access 同义：预留之前不许拦，那时按需倍增。
     */
    if (audit->capacity_sealed)
    {
        if (audit->access_count >= audit->sealed_capacity)
        {
            if (audit->omitted_pdo_samples != UINT64_MAX)
            {
                ++audit->omitted_pdo_samples;
            }
            return true;
        }
    }
    else if (audit->final_capacity != 0U &&
             audit->access_count >= audit->access_capacity)
    {
        if (audit->omitted_final_samples != UINT64_MAX)
        {
            ++audit->omitted_final_samples;
        }
        return true;
    }
    access = append_access(audit);
    if (access == NULL)
    {
        return false;
    }
    access->order = audit->next_order++;
    *last_record = audit->access_count - 1U;
    access->first_exchange = exchange;
    access->last_exchange = exchange;
    access->sample_count = 1U;
    access->phase = phase;
    access->transport = EMASTER_AUDIT_TRANSPORT_PDO;
    access->direction = direction;
    access->value_kind = value_kind;
    access->slave_position = slave_position;
    access->index = index;
    access->subindex = subindex;
    access->bit_length = bit_length;
    access->bit_offset = bit_offset;
    access->succeeded = succeeded;
    access->unsigned_value = unsigned_value;
    access->signed_value = signed_value;
    return true;
}
