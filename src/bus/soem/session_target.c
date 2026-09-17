#include "session_internal.h"

#include <string.h>

static bool target_within_limits(
    const emaster_control_session_axis_result_t *axis,
    int32_t target)
{
    if (axis == NULL || !axis->software_position_limits_read)
    {
        return false;
    }
    /* 最小值等于最大值时，设备没有提供可用的位置区间。 */
    if (axis->software_position_limit_min == axis->software_position_limit_max)
    {
        return true;
    }
    return target >= axis->software_position_limit_min &&
           target <= axis->software_position_limit_max;
}

emaster_control_session_status_t emaster_soem_session_position_target_step(
    emaster_soem_session_t *session,
    bool *updated)
{
    emaster_position_target_source_result_t source_result;
    size_t axis_index;

    if (session == NULL || session->plan == NULL || session->report == NULL ||
        session->position_target_source == NULL ||
        session->position_target_source_targets == NULL ||
        updated == NULL || session->plan->axes == NULL)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }
    *updated = false;

    /* P5.2: 验证所有轴都在支持的模式（CSP/CSV/CST） */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        int8_t mode;
        if (session->plan->axes[axis_index].operation_mode == NULL)
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
        mode = session->plan->axes[axis_index].operation_mode->value;
        if (mode != INT8_C(8) && mode != INT8_C(9) && mode != INT8_C(10))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }

    /* 调用回调获取新目标 */
    source_result = session->position_target_source(
        session->report->cycle_count, session->axes, session->plan->axis_count,
        session->position_target_source_targets, session->plan->axis_count,
        session->position_target_source_user_data);

    if (source_result == EMASTER_POSITION_TARGET_SOURCE_HOLD)
    {
        return EMASTER_CONTROL_SESSION_OK;
    }
    if (source_result != EMASTER_POSITION_TARGET_SOURCE_UPDATED)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }

    /* 后验证：检查回调返回的所有目标 */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        int32_t target = session->position_target_source_targets[axis_index];
        const emaster_session_axis_plan_t *axis_plan = &session->plan->axes[axis_index];
        int8_t mode = axis_plan->operation_mode != NULL ? axis_plan->operation_mode->value
                                                        : INT8_C(0);

        /* 软件限位检查 */
        if (mode == INT8_C(8) && !target_within_limits(&session->axes[axis_index], target))
        {
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }

        /*
         * C3：力矩模式（10）的上限，单位是额定力矩的千分比。模式 10 下前面两条位置
         * 量纲的检查都失效（软限位本就不进门，单步限幅的阈值是 counts，±1000 永远
         * 够不着），所以这是力矩唯一的数值护栏。超限与单步超限同判：MOTION_INVALID，
         * 沿用 D4 已定的"整会话中止"语义。0 = 未配置，不启用。
         */
        if (mode == INT8_C(10) && session->torque_target_limit_per_mille > 0U)
        {
            int64_t limit = (int64_t)session->torque_target_limit_per_mille;
            if ((int64_t)target > limit || (int64_t)target < -limit)
            {
                return EMASTER_CONTROL_SESSION_MOTION_INVALID;
            }
        }

        /* 单步限幅检查：拒绝相邻两条目标差超过阈值的命令。
         * 必须与主站上一次写出的目标（607A 历史）比较，而非 PDO 读回的 6064。
         * position_target_committed 为 false 时（首条目标尚未提交），target_positions
         * 尚无有效历史基准，跳过检查；驱动器使能可能在任意周期才完成，
         * 不能用 cycle_count > 0 代替。
         * C4：阈值是位置计数，模式 10 下与目标量纲不同（千分比），跳过。 */
        if (mode != INT8_C(10) && session->position_target_max_step_counts > 0U &&
            session->position_target_committed)
        {
            int64_t delta = (int64_t)target -
                            (int64_t)session->target_positions[axis_index];
            uint64_t abs_delta = (uint64_t)(delta < 0 ? -delta : delta);
            if (abs_delta > session->position_target_max_step_counts)
            {
                return EMASTER_CONTROL_SESSION_MOTION_INVALID;
            }
        }
    }

    /* 硬件写入：先验证所有轴写入成功 */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (!emaster_soem_axis_set_target_value(
                &session->plan->axes[axis_index], &session->axes[axis_index],
                session->position_target_source_targets[axis_index]))
        {
            /* 硬件写入失败：不更新会话状态 */
            return EMASTER_CONTROL_SESSION_MOTION_INVALID;
        }
    }

    /* 状态提交：仅在所有硬件写入成功后更新会话状态 */
    memcpy(session->target_positions, session->position_target_source_targets,
           session->plan->axis_count * sizeof(*session->target_positions));
    session->position_target_committed = true;

    *updated = true;
    return EMASTER_CONTROL_SESSION_OK;
}
