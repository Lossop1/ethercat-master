#ifndef EMASTER_MULTIAXIS_COORDINATOR_H
#define EMASTER_MULTIAXIS_COORDINATOR_H

#include "emaster/cia402/controller.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 多轴帧处理结果；任何一轴不满足门槛都不得发布部分成功的帧。 */
typedef enum
{
    EMASTER_MULTIAXIS_OK = 0,
    EMASTER_MULTIAXIS_INVALID_ARGUMENT,
    EMASTER_MULTIAXIS_AXIS_COUNT_MISMATCH,
    EMASTER_MULTIAXIS_SEQUENCE_NOT_MONOTONIC,
    EMASTER_MULTIAXIS_DEADLINE_EXPIRED,
    EMASTER_MULTIAXIS_STATUS_UNKNOWN,
    EMASTER_MULTIAXIS_CONTROLLER_INVALID
} emaster_multiaxis_status_t;

/*
 * 协调器只引用调用者预先分配的单轴控制器数组，不拥有或分配任何运行时存储；初始化必须
 * 在周期线程启动前完成，周期线程只能由一个所有者调用 step。
 */
typedef struct
{
    emaster_cia402_controller_t *controllers;
    size_t axis_count;
    uint64_t last_sequence;
    bool has_last_sequence;
} emaster_multiaxis_coordinator_t;

/* 一次完整周期帧的输入和输出；数组长度必须都等于协调器配置的轴数。 */
typedef struct
{
    uint64_t sequence;
    uint64_t deadline_ns;
    size_t axis_count;
    const uint16_t *status_words;
    emaster_cia402_output_t *outputs;
} emaster_multiaxis_frame_t;

/* 配置并初始化协调器；调用者继续拥有 controllers 数组。 */
bool emaster_multiaxis_coordinator_init(
    emaster_multiaxis_coordinator_t *coordinator,
    emaster_cia402_controller_t *controllers,
    size_t axis_count);

/* 在周期边界一次处理完整轴集合；now_ns 由外部时钟提供，函数不读取系统时钟。 */
emaster_multiaxis_status_t emaster_multiaxis_coordinator_step(
    emaster_multiaxis_coordinator_t *coordinator,
    const emaster_multiaxis_frame_t *frame,
    uint64_t now_ns);

#endif
