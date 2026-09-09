#ifndef EMASTER_MOTION_POSITION_TARGET_H
#define EMASTER_MOTION_POSITION_TARGET_H

#include "emaster/motion/relative_position.h"

#include <stdint.h>

/* 周期位置目标源的最小结果，不包含输入频率和传输层语义。 */
typedef enum
{
    EMASTER_POSITION_TARGET_SOURCE_HOLD = 0,
    EMASTER_POSITION_TARGET_SOURCE_UPDATED,
    EMASTER_POSITION_TARGET_SOURCE_INVALID
} emaster_position_target_source_result_t;

/* 角度目标可以相对当前参考位置，也可以相对明确的绝对零点。 */
typedef enum
{
    EMASTER_POSITION_TARGET_RELATIVE = 0,
    EMASTER_POSITION_TARGET_ABSOLUTE
} emaster_position_target_kind_t;

/*
 * 单轴坐标模型由调用者完整提供，不保存全局零点，也不使用设备目录默认换算值。
 * absolute_zero_counts 是工程零度在设备计数坐标中的位置；limits_enabled 为 false 时不检查
 * 本地限位，驱动器实际读回的软件限位仍由控制会话独立检查。
 */
typedef struct
{
    emaster_motion_coordinate_t coordinate;
    emaster_position_scale_t scale;
    int8_t polarity;
    int32_t absolute_zero_counts;
    bool limits_enabled;
    int32_t minimum_counts;
    int32_t maximum_counts;
} emaster_position_target_axis_model_t;

typedef enum
{
    EMASTER_POSITION_TARGET_OK = 0,
    EMASTER_POSITION_TARGET_INVALID_ARGUMENT,
    EMASTER_POSITION_TARGET_INVALID_SCALE,
    EMASTER_POSITION_TARGET_LIMIT_VIOLATION,
    EMASTER_POSITION_TARGET_OUT_OF_RANGE
} emaster_position_target_status_t;

/* 根据单轴坐标模型生成 PDO 所需的原始位置计数。 */
emaster_position_target_status_t emaster_position_target_from_angle(
    const emaster_position_target_axis_model_t *model,
    emaster_position_target_kind_t kind,
    int32_t reference_counts,
    int32_t angle_millidegrees,
    int32_t *target_counts);

#endif
