/*
 * 审计容量两段语义的回归测试：解封之后容量用尽，只能计数，不许再分配。
 *
 * 现场来源（2026-09-17 五轴停机入口，报告 archive/…-run-p85b.json）：审计数组在
 * 周期阶段正好写满（cyclic_capacity = 960895 条，约 84.6 MB），停机序言为保留故障
 * 阶段的样本把数组解封，解封后的第一条记录就让这块 84.6 MB 的内存翻倍——内核侧
 * mremap 实测 3.757 ms CPU，正好落在周期尾部，下一帧晚发 4 ms，五轴同步看门狗全部
 * 跳闸（AL 0x1A、WKC 由 12 掉到 5）。
 *
 * 测试复刻的是这一次故障的调用次序，而不是直接调某个函数：
 *   reserve(总量) -> seal_cyclic(水线) -> 周期阶段写满水线 -> end_cyclic(解封)
 *   -> 停机诊断继续写 -> 写满总量 -> 再写一条
 *
 * 断言的性质：
 *   0. **预留之前**（final_capacity 还是 0）记录一条都不能丢：那时两个计数都是 0，
 *      "满了"的判据如果写成 `access_count >= access_capacity`，`0 >= 0` 成立，会话
 *      建立阶段（PRE-OP 配置读写）的每一条记录都会被当成溢出——台架上实测丢了 255 条
 *      配置轨迹。这一条是给这个错留的回归网。
 *   1. 封存状态下撞上水线的记录：条数不变、容量不变、返回真（"不记"不等于"失败"），
 *      且计入 omitted_pdo_samples；水线之内的记录一条不少；
 *   2. 解封后能写满留给停机的那一段，一条不少——这正是原先触发扩容的地方；
 *   3. 总量用尽后再追加：只计数（进 omitted_final_samples），且 access_capacity
 *      **原地不动**。容量不变即没有走扩容分支，这是"周期路径里不再分配"的硬证据；
 *      修改前这里会翻倍成 2 倍总量，第 3 条断言必然失败。
 *
 * 编译与运行（本机 MinGW 与 Pi 都适用；不依赖 SOEM、不访问总线）：
 *   gcc -std=c11 -O2 -Iinclude -o tmp/test_audit_capacity \
 *       tests/unit/audit/test_audit_capacity.c src/audit/run_audit.c
 *   ./tmp/test_audit_capacity
 */
#include "emaster/audit/run_audit.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

enum
{
    /* 总量、周期段水线、差额（留给停机诊断的那一段）。取小值只为让断言读得出来，
     * 三段的关系才是被测对象：水线 < 总量，差额 = 总量 - 水线。 */
    TOTAL_CAPACITY = 40,
    CYCLIC_WATERMARK = 24,
    FINAL_MARGIN = TOTAL_CAPACITY - CYCLIC_WATERMARK
};

static unsigned int failures;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        printf("  [不符] %s\n", what);
    }
}

/* 写一条 PDO 记录；值随 idx 变，避免被合并进上一条。 */
static bool write_pdo(emaster_run_audit_t *audit, size_t *last_record, uint64_t exchange)
{
    return emaster_run_audit_record_pdo(
        audit, last_record, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION,
        EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED, UINT16_C(1),
        UINT16_C(0x6064), UINT8_C(0), UINT8_C(32), UINT32_C(0), exchange, true,
        exchange, (int64_t)exchange);
}

/* 写一条停机诊断记录（SDO 读），每次都不一样，同样不会被合并。 */
static bool write_diagnostic(emaster_run_audit_t *audit, uint16_t index)
{
    return emaster_run_audit_record_access(
        audit, EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC, EMASTER_AUDIT_TRANSPORT_SDO,
        EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED, UINT16_C(1), index,
        UINT8_C(0), UINT8_C(16), UINT32_C(0), UINT64_C(1), &index, sizeof(index), true,
        index, (int64_t)index);
}

/* 写一条会话建立阶段的配置记录（PRE-OP 的 SDO 写）。 */
static bool write_config(emaster_run_audit_t *audit, uint16_t index)
{
    uint16_t value = index;

    return emaster_run_audit_record_access(
        audit, EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, EMASTER_AUDIT_TRANSPORT_SDO,
        EMASTER_AUDIT_DIRECTION_WRITE, EMASTER_AUDIT_VALUE_UNSIGNED, UINT16_C(1), index,
        UINT8_C(0), UINT8_C(16), UINT32_C(0), UINT64_C(0), &value, sizeof(value), true,
        value, (int64_t)value);
}

int main(void)
{
    emaster_run_audit_t audit;
    size_t last_record = SIZE_MAX;
    size_t reserved_capacity;
    size_t index;

    emaster_run_audit_init(&audit);
    printf("审计容量两段语义：总量=%d，周期水线=%d，停机差额=%d\n", TOTAL_CAPACITY,
           CYCLIC_WATERMARK, FINAL_MARGIN);

    /*
     * 先跑"还没预留"那一段：配置阶段的记录必须照常写进数组、按需倍增。
     * 台架上那 255 条配置轨迹就是在这里丢的。
     */
    printf("预留之前：配置阶段的记录一条都不能丢\n");
    for (index = 0U; index < 3U; ++index)
    {
        if (!write_config(&audit, (uint16_t)(0x1000U + index)))
        {
            ++failures;
            printf("  [不符] 预留之前第 %u 条应当记下\n", (unsigned int)(index + 1U));
            break;
        }
    }
    check(audit.access_count == 3U, "预留之前不得丢记录");
    check(audit.omitted_final_samples == 0U, "预留之前不得计溢出");
    check(audit.omitted_pdo_samples == 0U, "预留之前不得计周期段溢出");
    check(!audit.allocation_failed, "预留之前的按需倍增不得算失败");
    check(emaster_run_audit_reserve(&audit, TOTAL_CAPACITY + audit.access_count),
          "把已记条数带上再预留应当成功");
    printf("（预留前已记 %u 条，下面按两段语义继续）\n", (unsigned int)audit.access_count);
    emaster_run_audit_destroy(&audit);

    emaster_run_audit_init(&audit);
    printf("预留与封存\n");
    check(emaster_run_audit_reserve(&audit, TOTAL_CAPACITY), "预留应当成功");
    reserved_capacity = audit.access_capacity;
    check(reserved_capacity == TOTAL_CAPACITY, "预留后容量应等于请求值");
    check(audit.final_capacity == TOTAL_CAPACITY, "预留后应记下总量上限");
    emaster_run_audit_seal_cyclic(&audit, CYCLIC_WATERMARK);
    check(audit.capacity_sealed, "封存后应处于封存态");
    check(audit.sealed_capacity == CYCLIC_WATERMARK, "水线应等于封存时传入的值");

    printf("周期段：写满水线\n");
    for (index = 0U; index < CYCLIC_WATERMARK; ++index)
    {
        if (!write_pdo(&audit, &last_record, (uint64_t)(index + 1U)))
        {
            ++failures;
            printf("  [不符] 水线之内第 %u 条应当记下\n", (unsigned int)(index + 1U));
            break;
        }
    }
    check(audit.access_count == CYCLIC_WATERMARK, "水线之内的记录一条不少");
    check(audit.omitted_pdo_samples == 0U, "水线之内不应有截断");

    printf("周期段：超出水线只计数\n");
    check(write_pdo(&audit, &last_record, UINT64_C(1000)), "超出水线的记录应返回真");
    check(audit.access_count == CYCLIC_WATERMARK, "超出水线的记录不得写进数组");
    check(audit.omitted_pdo_samples == 1U, "超出的 PDO 样本应计入 omitted_pdo_samples");
    check(audit.access_capacity == reserved_capacity, "超出水线不得改变容量");
    check(write_diagnostic(&audit, UINT16_C(0x603F)),
          "超出水线的诊断记录应返回真");
    check(audit.access_count == CYCLIC_WATERMARK, "超出水线的诊断记录不得写进数组");
    check(audit.omitted_pdo_samples == 2U,
          "周期段吃满只记一笔账：诊断记录同样进 omitted_pdo_samples");

    printf("解封：停机诊断写满预留的差额\n");
    emaster_run_audit_end_cyclic(&audit);
    check(!audit.capacity_sealed, "解封后不应再处于封存态");
    for (index = 0U; index < FINAL_MARGIN; ++index)
    {
        if (!write_diagnostic(&audit, (uint16_t)(0x2000U + index)))
        {
            ++failures;
            printf("  [不符] 停机段第 %u 条应当记下\n", (unsigned int)(index + 1U));
            break;
        }
    }
    check(audit.access_count == TOTAL_CAPACITY, "解封后应能写满留给停机的那一段");
    check(audit.access_capacity == reserved_capacity,
          "写满预留总量也不得改变容量（这正是 P8.5 触发扩容的地方）");
    check(audit.final_capacity == reserved_capacity, "总量上限不得被抬高");
    check(audit.omitted_final_samples == 0U, "停机段之内不应有截断");

    printf("总量用尽：只计数，不再分配\n");
    check(write_diagnostic(&audit, UINT16_C(0x7000)), "总量用尽后应返回真而非失败");
    check(audit.access_count == TOTAL_CAPACITY, "总量用尽的记录不得写进数组");
    check(audit.omitted_final_samples == 1U,
          "用尽后的记录应计入 omitted_final_samples");
    check(audit.access_capacity == reserved_capacity,
          "总量用尽后容量必须原地不动——容量不变即没有走扩容分支");
    check(!audit.allocation_failed, "容量用尽不是分配失败，不得污染分配失败标志");

    emaster_run_audit_destroy(&audit);
    if (failures != 0U)
    {
        printf("审计容量回归：%u 项不符\n", failures);
        return 1;
    }
    printf("审计容量回归：全部通过\n");
    return 0;
}
