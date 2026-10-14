#include "emaster/motion/target_guard.h"

emaster_target_guard_result_t emaster_target_guard_check(
    const int32_t *targets,
    const int32_t *actual_positions,
    const uint64_t *limits_counts,
    size_t axis_count,
    size_t *offending_axis,
    uint64_t *offending_offset)
{
    size_t axis_index;

    if (axis_count == 0U)
    {
        return EMASTER_TARGET_GUARD_OK;
    }
    /*
     * 调用方在进入这里之前已经校验过会话字段非空。这里的判空不是重复校验，
     * 是让实时线程不会因为一个空指针直接崩——崩在周期线程里没有任何恢复路径。
     */
    if (targets == NULL || actual_positions == NULL || limits_counts == NULL)
    {
        return EMASTER_TARGET_GUARD_OK;
    }

    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        int64_t delta;
        uint64_t offset;

        if (limits_counts[axis_index] == 0U)
        {
            continue; /* 该轴不判：换算不可用，或未配置 */
        }

        delta = (int64_t)targets[axis_index] - (int64_t)actual_positions[axis_index];
        offset = (uint64_t)(delta < 0 ? -delta : delta);
        if (offset > limits_counts[axis_index])
        {
            if (offending_axis != NULL)
            {
                *offending_axis = axis_index;
            }
            if (offending_offset != NULL)
            {
                *offending_offset = offset;
            }
            return EMASTER_TARGET_GUARD_TOO_FAR;
        }
    }

    return EMASTER_TARGET_GUARD_OK;
}
