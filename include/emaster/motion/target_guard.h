#ifndef EMASTER_MOTION_TARGET_GUARD_H
#define EMASTER_MOTION_TARGET_GUARD_H

#include <stddef.h>
#include <stdint.h>

/*
 * 外部目标的合理性判据：目标与**当前实际位置**的距离。
 *
 * 这不是限幅，也不是速度限制。主站不裁剪客户端给的值，也不替它规划；只在一条目标
 * 明显不合理时把它整条丢掉。判据是一个**距离**（counts），因此与客户端发送频率无关
 * ——这正是它取代原先那道单步闸的原因：后者比的是相邻两条目标的差，同一个配置在
 * 200 Hz 与 1 kHz 下允许的速度差四倍，而"允许多快"本来就不是主站该定的数。
 *
 * 用距离而不是增量还有一个直接后果：客户端正常快速跟随时，目标与实际位置本来就贴得
 * 很近，不会误伤；只有符号翻转、未初始化值、量纲错位这类才会撞上。
 *
 * 限幅值逐轴给。每轴用自己的换算（编码器分辨率 × 减速比）折算，所以 25:1 与 28:1 的
 * 轴拿到各自的值。limits_counts[i] == 0 表示该轴不判（换算不可用，或未配置）。
 */
typedef enum
{
    EMASTER_TARGET_GUARD_OK = 0,
    EMASTER_TARGET_GUARD_TOO_FAR
} emaster_target_guard_result_t;

/*
 * 逐轴比较目标与实际位置。返回 TOO_FAR 时，*offending_axis 是第一个越界的轴号，
 * *offending_offset 是它的偏移量（counts），供报告记录。两个出参可为 NULL。
 *
 * 判据针对整条命令（全轴快照），不是单轴：一条命令代表所有轴在同一时刻的目标，
 * 只放行其中一部分会让轴与轴之间失去同步。
 */
emaster_target_guard_result_t emaster_target_guard_check(
    const int32_t *targets,
    const int32_t *actual_positions,
    const uint64_t *limits_counts,
    size_t axis_count,
    size_t *offending_axis,
    uint64_t *offending_offset);

#endif
