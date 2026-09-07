#ifndef EMASTER_MOTION_VELOCITY_PROFILE_H
#define EMASTER_MOTION_VELOCITY_PROFILE_H

#include "emaster/config/runtime_config.h"
#include "emaster/motion/relative_position.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CSV 周期速度轨迹的单轴运行状态；所有数组由会话层预分配。 */
typedef struct
{
    int32_t command_velocity;
    int32_t target_velocity;
    uint64_t max_velocity_error;
    uint64_t acceleration;
    uint64_t deceleration;
    uint64_t acceleration_remainder;
    uint64_t deceleration_remainder;
    uint64_t elapsed_cycles;
    uint64_t elapsed_settle_cycles;
} emaster_velocity_axis_t;

typedef struct
{
    emaster_velocity_axis_t *axes;
    size_t axis_count;
    uint64_t duration_cycles;
    uint64_t settle_cycles;
    uint64_t elapsed_cycles;
    uint64_t elapsed_settle_cycles;
    uint32_t cycle_ns;
    bool initialized;
} emaster_velocity_motion_t;

bool emaster_velocity_motion_init(const emaster_motion_profile_t *profile,
                                  const emaster_motion_axis_config_t *const *axis_configs,
                                  const emaster_position_scale_t *scales,
                                  size_t axis_count, uint32_t cycle_ns,
                                  emaster_velocity_axis_t *axis_storage,
                                  emaster_velocity_motion_t *motion);

emaster_relative_motion_status_t emaster_velocity_motion_step(
    emaster_velocity_motion_t *motion, const int32_t *actual_velocities,
    int32_t *target_velocities, size_t axis_count);

#endif
