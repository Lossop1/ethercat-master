#ifndef EMASTER_CYCLIC_HISTOGRAM_H
#define EMASTER_CYCLIC_HISTOGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 周期线程内可用的定桶直方图：记录只做一次除法和一次自增，无分配、无遍历，
 * 因此可以在实时路径调用。百分位在报告阶段（非实时）由桶计数反推。
 *
 * 桶宽是绝对纳秒值而非按周期缩放：baseline 的判据本身是绝对量
 * （send_lateness 上限 100us、round_trip 上限为周期时间），跟着周期缩放会让
 * 不同周期下的报告无法直接比较。
 *
 * 落在量程外的样本进入 underflow/overflow 计数，不被丢弃；真实极值另由
 * min/max 保留，因此量程选择只影响百分位分辨率，不影响极值的正确性。
 */
#define EMASTER_HISTOGRAM_BUCKET_COUNT 512U

typedef struct
{
    /* 第 0 桶的下界，含。有符号指标取负值使量程以零为中心。 */
    int64_t origin_ns;
    /* 单桶宽度，纳秒。必须为正。 */
    int64_t bucket_width_ns;
    uint64_t buckets[EMASTER_HISTOGRAM_BUCKET_COUNT];
    uint64_t underflow_count;
    uint64_t overflow_count;
    uint64_t sample_count;
} emaster_cyclic_histogram_t;

/* 百分位查询结果。桶宽带来的不确定度显式给出，避免把 p99.9 当作精确值。 */
typedef struct
{
    bool present;
    /* 落在桶内时为桶下界；落在量程外时为 true 并改用调用方的 min/max。 */
    bool below_range;
    bool above_range;
    int64_t lower_bound_ns;
    int64_t upper_bound_ns;
} emaster_cyclic_histogram_quantile_t;

void emaster_cyclic_histogram_init(emaster_cyclic_histogram_t *histogram,
                                   int64_t origin_ns, int64_t bucket_width_ns);

/* 记录一个样本。实时路径调用，恒定时间。 */
void emaster_cyclic_histogram_record(emaster_cyclic_histogram_t *histogram,
                                     int64_t value_ns);

/*
 * 求分位点。numerator/denominator 表达分位，例如 999/1000 表示 p99.9，
 * 避免浮点参与判定。报告阶段调用，允许遍历全部桶。
 */
emaster_cyclic_histogram_quantile_t emaster_cyclic_histogram_quantile(
    const emaster_cyclic_histogram_t *histogram,
    uint64_t numerator,
    uint64_t denominator);

#endif
