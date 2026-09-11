#include "emaster/cyclic/histogram.h"

#include <string.h>

void emaster_cyclic_histogram_init(emaster_cyclic_histogram_t *histogram,
                                   int64_t origin_ns, int64_t bucket_width_ns)
{
    if (histogram == NULL)
    {
        return;
    }
    memset(histogram, 0, sizeof(*histogram));
    histogram->origin_ns = origin_ns;
    /* 宽度非正会让记录退化为除零；退回 1ns 保证结构始终可用。 */
    histogram->bucket_width_ns = bucket_width_ns > 0 ? bucket_width_ns : 1;
}

void emaster_cyclic_histogram_record(emaster_cyclic_histogram_t *histogram,
                                     int64_t value_ns)
{
    int64_t offset;
    int64_t index;

    if (histogram == NULL || histogram->bucket_width_ns <= 0)
    {
        return;
    }
    ++histogram->sample_count;
    if (value_ns < histogram->origin_ns)
    {
        ++histogram->underflow_count;
        return;
    }
    offset = value_ns - histogram->origin_ns;
    index = offset / histogram->bucket_width_ns;
    if (index >= (int64_t)EMASTER_HISTOGRAM_BUCKET_COUNT)
    {
        ++histogram->overflow_count;
        return;
    }
    ++histogram->buckets[(size_t)index];
}

emaster_cyclic_histogram_quantile_t emaster_cyclic_histogram_quantile(
    const emaster_cyclic_histogram_t *histogram,
    uint64_t numerator,
    uint64_t denominator)
{
    emaster_cyclic_histogram_quantile_t result;
    uint64_t threshold;
    uint64_t cumulative;
    size_t index;

    memset(&result, 0, sizeof(result));
    if (histogram == NULL || denominator == 0U ||
        numerator > denominator || histogram->sample_count == 0U)
    {
        return result;
    }
    result.present = true;
    /*
     * 取满足 累计计数 >= ceil(sample_count * numerator / denominator) 的第一个桶。
     * 先乘后除，且乘法在 uint64 内先判溢出，避免大样本量下的截断。
     */
    if (numerator != 0U && histogram->sample_count > UINT64_MAX / numerator)
    {
        /* 溢出时退化为先除后乘，精度损失只影响阈值选桶，不影响量程判定。 */
        threshold = (histogram->sample_count / denominator) * numerator;
    }
    else
    {
        threshold = (histogram->sample_count * numerator + denominator - 1U) / denominator;
    }
    if (threshold == 0U)
    {
        threshold = 1U;
    }
    cumulative = histogram->underflow_count;
    if (cumulative >= threshold)
    {
        result.below_range = true;
        return result;
    }
    for (index = 0U; index < EMASTER_HISTOGRAM_BUCKET_COUNT; ++index)
    {
        cumulative += histogram->buckets[index];
        if (cumulative >= threshold)
        {
            result.lower_bound_ns =
                histogram->origin_ns + (int64_t)index * histogram->bucket_width_ns;
            result.upper_bound_ns = result.lower_bound_ns + histogram->bucket_width_ns;
            return result;
        }
    }
    result.above_range = true;
    return result;
}
