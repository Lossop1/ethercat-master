#ifndef EMASTER_MOTION_POSITION_COMMAND_H
#define EMASTER_MOTION_POSITION_COMMAND_H

#include "emaster/motion/relative_position.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * 位置命令只保存已经完成坐标换算的设备位置计数。
 * 单位换算、零点和极性由应用层或独立轨迹模块负责，不能在队列中猜测。
 */
typedef struct
{
    uint64_t sequence;
    /* 生效周期，包含该周期。 */
    uint64_t activate_cycle;
    /* 失效周期，不包含该周期；必须大于 activate_cycle。 */
    uint64_t expire_cycle;
} emaster_position_command_header_t;

typedef enum
{
    EMASTER_POSITION_COMMAND_UPDATED = 0, /* 已接受一条命令。 */
    EMASTER_POSITION_COMMAND_NO_COMMAND, /* 当前周期没有到期命令。 */
    EMASTER_POSITION_COMMAND_HOLD, /* 保持上一条仍有效的命令。 */
    EMASTER_POSITION_COMMAND_EXPIRED, /* 队列中的命令已失效。 */
    EMASTER_POSITION_COMMAND_INVALID_ARGUMENT, /* 调用参数或时间区间无效。 */
    EMASTER_POSITION_COMMAND_QUEUE_FULL, /* 队列没有可用槽位。 */
    EMASTER_POSITION_COMMAND_SEQUENCE_NOT_MONOTONIC, /* 命令序号未递增。 */
    EMASTER_POSITION_COMMAND_AXIS_COUNT_MISMATCH /* 命令轴数与队列不一致。 */
} emaster_position_command_status_t;

/* 角度命令可表示相对当前位置，或表示以绝对零点为基准的位置。 */
typedef enum
{
    EMASTER_POSITION_TARGET_OK = 0, /* 目标已完成换算并通过边界检查。 */
    EMASTER_POSITION_TARGET_INVALID_ARGUMENT, /* 模型、目标类型或输出参数无效。 */
    EMASTER_POSITION_TARGET_INVALID_SCALE, /* 没有可用的物理从站换算参数。 */
    EMASTER_POSITION_TARGET_LIMIT_VIOLATION, /* 目标越过调用者声明的限位。 */
    EMASTER_POSITION_TARGET_OUT_OF_RANGE /* 目标超出设备原始计数范围。 */
} emaster_position_target_status_t;

typedef enum
{
    EMASTER_POSITION_COMMAND_RELATIVE_TARGET = 0,
    EMASTER_POSITION_COMMAND_ABSOLUTE_TARGET
} emaster_position_command_target_kind_t;

/*
 * 单轴位置坐标模型由调用者完整提供，不保存全局零点，也不使用设备目录默认换算值。
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
} emaster_position_command_axis_model_t;

/* 根据单轴坐标模型生成绝对或相对目标，返回值仍为 PDO 使用的原始计数。 */
emaster_position_target_status_t emaster_position_command_target_from_angle(
    const emaster_position_command_axis_model_t *model,
    emaster_position_command_target_kind_t kind,
    int32_t reference_counts,
    int32_t angle_millidegrees,
    int32_t *target_counts);

/*
 * 单生产者、单消费者的有界位置命令队列。
 * position_storage 的容量必须是 slot_count * axis_count，按槽位连续排列。
 * 队列初始化后不得复制结构体；生产者和消费者各只能有一个线程。
 */
typedef struct
{
    emaster_position_command_header_t *headers;
    int32_t *position_storage;
    size_t slot_count;
    size_t axis_count;
    size_t write_index;
    size_t read_index;
    atomic_size_t pending_count;
    uint64_t last_published_sequence;
    uint64_t last_published_activate_cycle;
    uint64_t last_consumed_sequence;
    bool initialized;
} emaster_position_command_queue_t;

/* 队列返回给周期调用者的当前命令元数据。 */
typedef struct
{
    uint64_t sequence;
    uint64_t activate_cycle;
    uint64_t expire_cycle;
    size_t superseded_count;
    size_t expired_count;
} emaster_position_command_observation_t;

/*
 * 初始化不分配内存。headers 和 position_storage 必须在周期线程启动前保持有效，
 * slot_count 至少为 2，axis_count 必须与部署拓扑的轴数一致。
 */
bool emaster_position_command_queue_init(
    emaster_position_command_queue_t *queue,
    emaster_position_command_header_t *headers,
    int32_t *position_storage,
    size_t slot_count,
    size_t axis_count);

/*
 * 由唯一生产者发布一个完整的多轴命令。发布成功后，输入数组可以立即复用。
 * sequence 必须严格递增；activate_cycle 和 expire_cycle 使用主站周期序号。
 */
emaster_position_command_status_t emaster_position_command_queue_publish(
    emaster_position_command_queue_t *queue,
    const emaster_position_command_header_t *header,
    const int32_t *positions,
    size_t axis_count);

/*
 * 由唯一消费者取得当前周期已经生效的命令。
 * 同一周期前积压的命令只采用最后一个，其余命令计入 superseded_count；
 * 过期命令计入 expired_count。没有到生效周期时返回 NO_COMMAND。
 */
emaster_position_command_status_t emaster_position_command_queue_take(
    emaster_position_command_queue_t *queue,
    uint64_t current_cycle,
    int32_t *positions,
    size_t axis_capacity,
    emaster_position_command_observation_t *observation);

/*
 * 在停止生产和消费后清空待处理命令。序号不会回退，防止旧命令在重新启动时复用。
 */
void emaster_position_command_queue_clear(emaster_position_command_queue_t *queue);

/* 判断一个已经接收的命令在当前周期是否仍然有效。 */
bool emaster_position_command_is_active(uint64_t current_cycle,
                                        uint64_t expire_cycle);

#endif
