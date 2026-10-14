#include "session_internal.h"

#include <stdio.h>
#include <string.h>

#include "emaster/motion/target_guard.h"

/*
 * 外部目标路径上唯一保留的判据。
 *
 * 这条路径原先有三道闸：软限位、力矩上限、单步限幅。三道都已删除，理由是同一个：
 * 它们管的是"这个值合不合理"，而那属于客户端或上层；驱动器自己也有软限位与跟随
 * 误差保护，主站再插一层只会多一层看不见的滞后，而且两边都不知道对方改了什么。
 *
 * 更具体的两条实证：
 *   - 单步限幅比的是相邻两条**目标**的差。外部目标缓冲区是覆盖式最新值，客户端
 *     发得多快就决定了每拍差多少，于是同一个配置在 200 Hz 与 1 kHz 下允许的速度
 *     差四倍——"允许多快"本来就不是主站该定的数。
 *   - 软限位检查用的是 607D 读回来的区间，那是**电机侧**计数区间，而客户端的
 *     目标是同一套计数，看着对；但它的判据是"目标在不在区间内"，与客户端是否
 *     跟得上无关，一台没回零过的轴会被它整会话打死。
 *
 * 保留的这一条是距离：|目标 - 当前实际位置| 逐轴比上限。它不看增量，所以与客户端
 * 发送频率无关；正常快速跟随时目标本就贴着实际位置，不会误伤；只有符号翻转、
 * 未初始化值、量纲错位这类才会撞上。判据本身在 src/motion/target_guard.c，
 * 是纯函数，离线单测钉着它。
 *
 * 被拒不是失败：返回 TARGET_REJECTED，由调用方把这一条丢掉、会话继续跑。
 */
emaster_control_session_status_t emaster_soem_session_position_target_step(
    emaster_soem_session_t *session,
    bool *updated)
{
    emaster_position_target_source_result_t source_result;
    size_t axis_index;
    size_t offending_axis = 0U;
    uint64_t offending_offset = 0U;

    if (session == NULL || session->plan == NULL || session->report == NULL ||
        session->position_target_source == NULL ||
        session->position_target_source_targets == NULL ||
        updated == NULL || session->plan->axes == NULL)
    {
        return EMASTER_CONTROL_SESSION_MOTION_INVALID;
    }
    *updated = false;

    /*
     * 这条通道只认有周期目标的模式。判据是运行方案声明的量：声明 none 的模式（回零）
     * 没有周期目标，所以带这种轴的部署压根不会走到这里——会话在挂回调那一步就把这条
     * 通道整个放弃了（control_session.c）。这道检查留着，是为了任何调用者都不可能靠
     * "绕过绑定"造出一个回零轴在收位置目标的会话。
     *
     * 不按模式号认：同一个号在不同厂商的说明书里指的不是同一件事。
     */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        if (session->plan->axes[axis_index].operation_mode == NULL ||
            session->plan->axes[axis_index].operation_mode->quantity ==
                EMASTER_MODE_QUANTITY_NONE)
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

    /*
     * 后验证：整条命令一起过判据。一条命令代表所有轴在同一时刻的目标，只放行其中
     * 一部分会让轴与轴之间失去同步，所以拒就是整条拒。
     *
     * 上限逐轴来自会话建立时的折算（session_start.c），0 表示该轴不判。
     */
    if (emaster_target_guard_check(
            session->position_target_source_targets, session->actual_positions,
            session->external_target_limits_counts, session->plan->axis_count,
            &offending_axis, &offending_offset) != EMASTER_TARGET_GUARD_OK)
    {
        /*
         * 现场每拍都刷新，计数只在"从放行进入被拒"那一次加一。
         *
         * 为什么计数不能按拍：缓冲区是覆盖式最新值，主站每拍都读到同一条坏目标。
         * 1 kHz 下按拍计数，一个客户端发出去就不管的坏值会变成几百万次，那一列
         * 就没有任何可读性。按"被拒的段"计，客户端反复发同一个坏值只算一次，
         * 中间夹一条好目标再坏一次才算第二次——那才是"发生了几次"。
         */
        session->report->target_rejection_axis = offending_axis;
        session->report->target_rejection_offset_counts = offending_offset;
        if (!session->target_rejection_active)
        {
            session->target_rejection_active = true;
            session->report->target_rejected_count += 1U;
            /*
             * 只在第一次打。这一条会以 1 kHz 重复（客户端不发新值就一直超距），
             * 每拍打一行会把主站日志淹掉，而那正是要留给人看的东西。
             */
            fprintf(stderr,
                    "[TARGET] 拒收外部目标：第 %zu 轴偏移 %llu counts 超过上限 %llu；"
                    "该条目标未采纳，会话继续\n",
                    offending_axis + 1U, (unsigned long long)offending_offset,
                    (unsigned long long)(session->external_target_limits_counts != NULL
                                             ? session->external_target_limits_counts[offending_axis]
                                             : 0U));
        }
        return EMASTER_CONTROL_SESSION_TARGET_REJECTED;
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
    session->target_rejection_active = false;

    *updated = true;
    return EMASTER_CONTROL_SESSION_OK;
}
