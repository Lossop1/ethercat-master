#ifndef EMASTER_MOTION_RELATIVE_POSITION_H
#define EMASTER_MOTION_RELATIVE_POSITION_H

#include "emaster/config/runtime_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 换算参数必须来自当前物理从站，不能使用设备目录中的默认值代替。 */
typedef struct
{
    bool read_succeeded;
    uint32_t encoder_increments;
    uint32_t encoder_motor_revolutions;
    uint32_t gear_motor_revolutions;
    uint32_t gear_shaft_revolutions;
} emaster_position_scale_t;

typedef enum
{
    EMASTER_RELATIVE_MOTION_ACTIVE = 0,
    EMASTER_RELATIVE_MOTION_SETTLING,
    EMASTER_RELATIVE_MOTION_COMPLETE,
    EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT,
    EMASTER_RELATIVE_MOTION_INVALID_SCALE,
    EMASTER_RELATIVE_MOTION_TARGET_OUT_OF_RANGE,
    EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR
} emaster_relative_motion_status_t;

/* 每轴状态由调用者预分配，周期路径不进行动态内存分配。 */
typedef struct
{
    int32_t start_position;
    int32_t final_position;
    int32_t command_position;
    uint64_t delta_counts;
    uint64_t interpolation_remainder;
    uint64_t max_following_error_counts;
    uint64_t max_observed_following_error_counts;
    int8_t direction;
} emaster_relative_motion_axis_t;

typedef struct
{
    const emaster_motion_profile_t *profile;
    emaster_relative_motion_axis_t *axes;
    size_t axis_count;
    uint64_t duration_cycles;
    uint64_t settle_cycles;
    uint64_t elapsed_motion_cycles;
    uint64_t elapsed_settle_cycles;
    bool initialized;
} emaster_relative_motion_t;

/* 比较已批准方案的换算前提与当前物理从站读回，供运动前门控和报告共同使用。 */
bool emaster_position_scale_matches(
    const emaster_motion_axis_config_t *axis_config,
    const emaster_position_scale_t *scale);

/*
 * 把配置角度换算为每轴有符号计数，并锁定启动位置和终点。axis_configs 已由会话计划按
 * 拓扑顺序解析；函数拒绝缺失比例、整数溢出、零误差边界和不完整轴集合。
 */
emaster_relative_motion_status_t emaster_relative_motion_init(
    const emaster_motion_profile_t *profile,
    const emaster_motion_axis_config_t *const *axis_configs,
    const emaster_position_scale_t *scales,
    const int32_t *initial_positions,
    size_t axis_count,
    uint32_t cycle_ns,
    emaster_relative_motion_axis_t *axis_storage,
    emaster_relative_motion_t *motion);

/*
 * 使用本周期反馈检查上一周期命令的跟随误差，再用整数余数累加生成下一周期目标。所有轴共享
 * 同一个进度；任一轴异常都会阻止继续发布变化目标。
 */
emaster_relative_motion_status_t emaster_relative_motion_step(
    emaster_relative_motion_t *motion,
    const int32_t *actual_positions,
    int32_t *target_positions,
    size_t axis_count);

#endif
