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
 * 换算参数。两个来路：当前物理从站的 608F/6091 读回，与设备档里声明的 conversion。
 * 谁生效由 emaster_position_scale_resolve 定，来源记在 source 里。两者都没有时
 * source 为 NONE：说不出来的换算不拿来动轴。
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
 * 设备档里声明的换算（conversion）落成运行时用的换算参数。这是**只做转换的纯函数**：
 * 声明齐全就成功，不齐全就失败。声明与设备自报谁生效由 emaster_position_scale_resolve
 * 定，不在这里。
 *
 * 三项齐全才算声明过：编码器每转多少计数、减速比的电机侧与负载侧。缺一项就不是换算，
 * 是一半的换算 —— 而按半份算出来的计数看着像真的，只会把轴按错的系数动起来。缺项时
 * 返回 false 且**不碰** scale，调用方据此把来源留成"没有"（报告里写 none）。
 *
 * encoder_motor_revolutions 固定为 1：608F:2 的语义是"多少个电机转对应
 * encoder_increments 个计数"，声明这条路径下只有 1 有意义 —— 写别的值等于把"编码器
 * 不装在电机轴上"这件事也说成声明里有的。
 */
bool emaster_position_scale_from_declaration(uint32_t encoder_counts_per_motor_revolution,
                                             uint32_t gear_motor_revolutions,
                                             uint32_t gear_shaft_revolutions,
                                             emaster_position_scale_t *scale);

/*
 * 定这一轴生效的换算：设备档声明齐全就用声明，声明缺项就退回设备自报。
 *
 * **为什么声明优先，而设备自报不是优先的**：驱动器会答，答的却可能是错的——台架从站 1
 * 自报 6091=28000，实测真值 28（差 1000 倍）。608F/6091 是驱动器的**说法**，不是对这台
 * 机器的测量；设备档里的 conversion 是拿到这台机器的人对着它写下的判据。两者冲突时按
 * 判据。这不是说自报不可信到该丢掉：调用方把自报值单独留着（报告与控制台都要写出来），
 * 这里只决定"轴按哪个比例动"。
 *
 * device_read 可以是来源为 NONE 的换算（设备没答），此时只有声明能救；声明也缺项就
 * 得到一份 NONE，后面那道门会拦。换一台设备、或厂商把固件修好，都不需要改代码。
 */
void emaster_position_scale_resolve(const emaster_position_scale_t *device_read,
                                    uint32_t declared_encoder_counts_per_motor_revolution,
                                    uint32_t declared_gear_motor_revolutions,
                                    uint32_t declared_gear_shaft_revolutions,
                                    emaster_position_scale_t *effective);

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
