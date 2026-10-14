#ifndef EMASTER_MOTION_RELATIVE_POSITION_H
#define EMASTER_MOTION_RELATIVE_POSITION_H

#include "emaster/config/runtime_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 一个事实值的来源。两种来源必须分得开：设备对象字典读回来的是设备的事实，设备档里
 * 写的是人的声明。报告按来源分开写 —— "读到了"与"声明过"不是一件事。
 * NONE 表示这个事实没有值，调用方一律当作不可用。
 */
typedef enum
{
    EMASTER_FACT_SOURCE_NONE = 0,
    EMASTER_FACT_SOURCE_DEVICE = 1,
    EMASTER_FACT_SOURCE_CONFIGURED = 2
} emaster_fact_source_t;

/*
 * 换算参数。优先取自当前物理从站（608F/6091）—— 那是这一台的实测事实。设备不实现
 * 这两个对象时（MT 的对象字典里就没有它们），设备档里声明的缺省值接手，来源记在
 * source 里。两者都没有时 source 为 NONE：说不出来的换算不拿来动轴。
 */
typedef struct
{
    emaster_fact_source_t source;
    uint32_t encoder_increments;
    uint32_t encoder_motor_revolutions;
    uint32_t gear_motor_revolutions;
    uint32_t gear_shaft_revolutions;
} emaster_position_scale_t;

/* 换算可用与否只看有没有来源；数值本身合法与否由各自的换算函数判（分子分母不为零）。 */
bool emaster_position_scale_is_usable(const emaster_position_scale_t *scale);

/*
 * 来源的显示名（简体中文），给控制台那几行用。报告里用的是 ASCII 标识
 * （device / configured / none），两者不能混：报告是给工具读的，控制台是给人的。
 * 有这一行，"换算读取=未通过"与后面两个非零的读数就不会同时出现在同一个操作员眼前。
 */
const char *emaster_fact_source_label(emaster_fact_source_t source);

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
 * 使用当前物理从站的换算参数，把工程角度转换为有符号设备计数。
 * angle_millidegrees 使用毫度，输出可以是负数；溢出或未完成换算参数读取时返回 false。
 */
bool emaster_motion_angle_to_counts(
    int32_t angle_millidegrees,
    emaster_motion_coordinate_t coordinate,
    const emaster_position_scale_t *scale,
    int64_t *counts);

/*
 * 检查轨迹终点是否落在驱动器当前有效的软件位置范围内
 * 最小值等于最大值时没有形成有效区间，按该设备默认配置不启用此项限制
 */
bool emaster_relative_motion_axis_within_software_limits(
    const emaster_relative_motion_axis_t *axis,
    int32_t minimum,
    int32_t maximum);

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
