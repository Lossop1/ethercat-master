#include "session_internal.h"

#include "emaster/config/runtime_config.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint64_t absolute_position_difference(int32_t left, int32_t right) {
    int64_t difference = (int64_t)left - (int64_t)right;

    return (uint64_t)(difference < 0 ? -difference : difference);
}

emaster_control_session_status_t emaster_soem_session_run(emaster_soem_session_t *session) {
    size_t axis_index;
    bool motion_initialized = false;
    bool enable_started = false;
    uint64_t mode_confirmation_deadline_cycle;
    uint64_t enable_deadline_cycle = 0U;
    emaster_control_session_status_t status = EMASTER_CONTROL_SESSION_OK;
    emaster_control_session_status_t safety_status = EMASTER_CONTROL_SESSION_OK;

    emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_ENABLING,
                                   EMASTER_CONTROL_SESSION_OK);

    mode_confirmation_deadline_cycle =
        session->report->cycle_count + session->transition_cycles;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        if (!emaster_cia_process_image_decode_input(&session->images[axis_index], slave->inputs,
                                                    slave->Ibytes, &mode_display, &status_word,
                                                    &actual_position) ||
            !emaster_cia_process_image_audit_input(
                &session->images[axis_index], &session->report->audit,
                EMASTER_AUDIT_PHASE_OPERATION_REQUEST, (uint16_t)(axis_index + 1U),
                session->exchange)) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }
    {
        while (true) {
            emaster_multiaxis_frame_t frame;
            uint64_t now_ns;
            uint64_t deadline_ns;
            bool all_axes_enabled = true;
            bool all_modes_confirmed = true;
            bool feedback_valid = true;
            bool safety_denied = false;

            status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
                int8_t mode_display;
                uint16_t status_word;
                int32_t actual_position;

                session->axes[axis_index].input_decoded = emaster_cia_process_image_decode_input(
                    &session->images[axis_index], slave->inputs, slave->Ibytes, &mode_display,
                    &status_word, &actual_position);
                if (!session->axes[axis_index].input_decoded) {
                    feedback_valid = false;
                }
                if (!session->axes[axis_index].input_decoded)
                {
                    continue;
                }
                if (!emaster_cia_process_image_audit_input(
                        &session->images[axis_index], &session->report->audit,
                        EMASTER_AUDIT_PHASE_CYCLIC_OPERATION, (uint16_t)(axis_index + 1U),
                        session->exchange)) {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    return status;
                }
                session->axes[axis_index].mode_display = mode_display;
                session->axes[axis_index].status_word = status_word;
                if (!emaster_soem_axis_set_feedback(&session->plan->axes[axis_index],
                                                    &session->axes[axis_index], actual_position))
                    feedback_valid = false;

                /* P4.3: 从 SDO 观测线程读取监控数据。
                 * 6078h (actual_current)、6079h (dc_link_voltage)、200Bh (温度) 未映射进 TxPDO
                 * （设备 ESI 将三个模块都声明为 Fixed="true"，且 supports_pdo_configuration=false）。
                 * observer_thread 以 ~50ms 周期通过 SDO 读取这些对象，加锁保护 sdo_* 字段。
                 * 此处周期线程读取这些值填充到报告字段，避免周期内 SDO 阻塞。 */
                pthread_mutex_lock(&session->observer_mutex);
                if (session->axes[axis_index].sdo_current_read)
                {
                    session->axes[axis_index].actual_current = session->axes[axis_index].sdo_current_6078h;
                }
                if (session->axes[axis_index].sdo_voltage_read)
                {
                    session->axes[axis_index].dc_link_voltage = session->axes[axis_index].sdo_voltage_6079h;
                }
                if (session->axes[axis_index].sdo_mosfet_temp_read)
                {
                    session->axes[axis_index].mosfet_temperature = session->axes[axis_index].sdo_mosfet_temp_200b01h;
                }
                if (session->axes[axis_index].sdo_motor_temp_read)
                {
                    session->axes[axis_index].motor_temperature = session->axes[axis_index].sdo_motor_temp_200b02h;
                }
                if (session->axes[axis_index].sdo_motor_speed_read)
                {
                    session->axes[axis_index].actual_velocity = session->axes[axis_index].sdo_motor_speed_200b08h;
                }
                if (session->axes[axis_index].sdo_speed_command_read)
                {
                    session->axes[axis_index].target_velocity = session->axes[axis_index].sdo_speed_command_200b09h;
                }
                pthread_mutex_unlock(&session->observer_mutex);

                session->status_words[axis_index] = status_word;
                {
                    emaster_cia402_status_t decoded_status;

                    (void)emaster_cia402_decode_status(status_word, &decoded_status);
                    session->axes[axis_index].status_warning = decoded_status.warning;
                    session->axes[axis_index].voltage_enabled = decoded_status.voltage_enabled;
                    session->axes[axis_index].remote = decoded_status.remote;
                    session->axes[axis_index].target_reached = decoded_status.target_reached;
                    session->axes[axis_index].internal_limit_active =
                        decoded_status.internal_limit_active;
                    session->axes[axis_index].mode_specific_status =
                        decoded_status.mode_specific_bits;
                    session->axes[axis_index].manufacturer_specific_status =
                        decoded_status.manufacturer_specific_bits;
                }
                if (session->plan->motion_profile != NULL) {
                    session->actual_positions[axis_index] = actual_position;
                }
            }

            if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &deadline_ns)) {
                return session->clock.deadline_missed
                           ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                           : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;
            }
            frame.sequence = session->report->cycle_count;
            frame.deadline_ns = deadline_ns;
            frame.axis_count = session->plan->axis_count;
            frame.status_words = session->status_words;
            frame.outputs = session->controller_outputs;
            if (emaster_multiaxis_coordinator_step(&session->coordinator, &frame, now_ns) !=
                EMASTER_MULTIAXIS_OK) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_latch_failure(session, status);
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];
                bool operation_enabled = session->controller_outputs[axis_index].observed_state ==
                                         EMASTER_CIA402_STATE_OPERATION_ENABLED;
                bool mode_control_ready;

                axis_result->control_word = session->controller_outputs[axis_index].control_word;
                axis_result->cia402_state = session->controller_outputs[axis_index].observed_state;
                axis_result->operation_enabled_seen |= operation_enabled;
                axis_result->switch_on_disabled_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCH_ON_DISABLED;
                axis_result->ready_to_switch_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
                axis_result->switched_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCHED_ON;
                if (!session->images[axis_index].tx_mode_available &&
                    axis_result->mode_display_sdo_read) {
                    axis_result->mode_display = axis_result->mode_display_sdo;
                }
                axis_result->mode_display_match =
                    session->images[axis_index].tx_mode_available
                        ? axis_result->mode_display ==
                              session->plan->axes[axis_index].operation_mode->value
                        : axis_result->mode_display_sdo_read &&
                              axis_result->mode_display ==
                                  session->plan->axes[axis_index].operation_mode->value;
                mode_control_ready = axis_result->mode_display_match;
                if (!session->images[axis_index].tx_mode_available)
                {
                    /* 固定 PDO 无法提供周期 6061，只能以 SAFE-OP 的 6060 写后读回作为使能前证据 */
                    mode_control_ready = axis_result->mode_command_sdo_read &&
                        axis_result->mode_command_sdo ==
                            session->plan->axes[axis_index].operation_mode->value;
                }
                if (!emaster_session_axis_mode_allows_control(&session->plan->axes[axis_index],
                                                              mode_control_ready)) {
                    all_modes_confirmed = false;
                }
                if (session->controller_outputs[axis_index].fault_present) {
                    status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                if (!session->controller_outputs[axis_index].state_known)
                {
                    emaster_soem_session_note_runtime_failure(session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
                                         &safety_status);
                    safety_denied = true;
                }
                if (axis_result->internal_limit_active)
                {
                    emaster_soem_session_note_runtime_failure(session,
                                         EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE,
                                         &safety_status);
                    safety_denied = true;
                }
                if (!operation_enabled) {
                    all_axes_enabled = false;
                }
                else if (!axis_result->voltage_enabled)
                {
                    emaster_soem_session_note_runtime_failure(session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
                                         &safety_status);
                    safety_denied = true;
                }
            }
            session->report->all_axes_enabled_reached |= all_axes_enabled;
            if (all_modes_confirmed && !enable_started)
            {
                enable_started = true;
                enable_deadline_cycle =
                    session->report->cycle_count + session->transition_cycles;
            }
            if (enable_started && !all_axes_enabled &&
                session->report->cycle_count >= enable_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            if (!all_modes_confirmed &&
                session->report->cycle_count >= mode_confirmation_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            if (feedback_valid && !safety_denied &&
                (session->plan->motion_profile != NULL ||
                 session->position_target_source != NULL) &&
                all_axes_enabled && all_modes_confirmed) {
                if (session->position_target_source != NULL)
                {
                    bool command_updated = false;

                    /*
                     * 先检查上一周期目标的跟随误差，再请求新目标。顺序与固定方案路径
                     * 一致：motion_step 先检查跟随误差，再计算下一周期目标。
                     * 调用者通过 callbacks->position_target_max_following_error_counts
                     * 设置上限；0 表示不启用此检查（不依赖固定方案时的向后兼容行为）。
                     */
                    if (motion_initialized &&
                        session->position_target_max_following_error_counts > 0U)
                    {
                        for (axis_index = 0U;
                             axis_index < session->plan->axis_count; ++axis_index)
                        {
                            uint64_t following_error;
                            int64_t diff = (int64_t)session->axes[axis_index].actual_position -
                                           (int64_t)session->target_positions[axis_index];
                            following_error = (uint64_t)(diff < 0 ? -diff : diff);
                            if (following_error >
                                session->axes[axis_index].max_observed_following_error_counts)
                            {
                                session->axes[axis_index].max_observed_following_error_counts =
                                    following_error;
                            }
                            if (following_error >
                                session->position_target_max_following_error_counts)
                            {
                                status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                                emaster_soem_session_note_runtime_failure(
                                    session, status, &safety_status);
                                safety_denied = true;
                                break;
                            }
                        }
                    }
                    if (!safety_denied)
                    {
                    status = emaster_soem_session_position_target_step(
                        session, &command_updated);
                    if (status != EMASTER_CONTROL_SESSION_OK)
                    {
                        emaster_soem_session_note_runtime_failure(
                            session, status, &safety_status);
                        safety_denied = true;
                    }
                    else if (command_updated && !motion_initialized)
                    {
                        motion_initialized = true;
                        session->report->motion_started = true;
                    }
                    }
                }
                else
                {
                emaster_relative_motion_status_t motion_status;

                if (!motion_initialized) {
                    if (!session->motion_prepared) {
                        status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                        return status;
                    }
                    motion_initialized = true;
                    session->report->motion_started = true;
                }
                if (session->plan->motion_profile->trajectory ==
                    EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION)
                {
                    motion_status = emaster_relative_motion_step(
                        &session->motion, session->actual_positions, session->target_positions,
                        session->plan->axis_count);
                }
                else if (session->plan->motion_profile->trajectory ==
                         EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY)
                {
                    motion_status = emaster_velocity_motion_step(
                        &session->velocity_motion, session->actual_positions,
                        session->target_positions, session->plan->axis_count);
                }
                else
                {
                    motion_status = EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
                }
                /* 即使本周期因跟随误差退出，也要把触发值保留到会话报告。 */
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    session->axes[axis_index].max_observed_following_error_counts =
                        session->motion_axes[axis_index].max_observed_following_error_counts;
                }
                if (motion_status == EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR) {
                    status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                else if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE &&
                         motion_status != EMASTER_RELATIVE_MOTION_SETTLING &&
                         motion_status != EMASTER_RELATIVE_MOTION_COMPLETE) {
                    status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                if (!safety_denied)
                {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        if (!emaster_soem_axis_set_target_value(
                                &session->plan->axes[axis_index], &session->axes[axis_index],
                                session->target_positions[axis_index]))
                            safety_denied = true;
                    }
                    session->report->motion_completed =
                        motion_status == EMASTER_RELATIVE_MOTION_COMPLETE;
                }
                if (session->report->motion_completed &&
                    session->plan->motion_profile->trajectory ==
                        EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION) {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        emaster_control_session_axis_result_t *axis_result =
                            &session->axes[axis_index];
                        const int32_t planned_angle =
                            session->motion_axis_configs[axis_index]->relative_angle_millidegrees;

                        axis_result->motion_completion_actual_position =
                            axis_result->actual_position;
                        axis_result->motion_actual_delta_counts =
                            (int64_t)axis_result->motion_completion_actual_position -
                            (int64_t)axis_result->initial_actual_position;
                        axis_result->motion_final_error_counts = absolute_position_difference(
                            axis_result->motion_completion_actual_position,
                            axis_result->motion_final_position);
                        axis_result->motion_direction_match =
                            planned_angle == 0
                                ? axis_result->motion_actual_delta_counts == 0
                                : (planned_angle > 0 &&
                                   axis_result->motion_actual_delta_counts > 0) ||
                                      (planned_angle < 0 &&
                                       axis_result->motion_actual_delta_counts < 0);
                        if (!axis_result->motion_direction_match ||
                            axis_result->motion_final_error_counts >
                                axis_result->max_following_error_counts) {
                            status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                            emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                            safety_denied = true;
                        }
                    }
                    if (safety_denied)
                    {
                        session->report->motion_completed = false;
                    }
                }
                }
            }
            {
                safety_status = emaster_soem_session_apply_safety(
                    session, feedback_valid, all_modes_confirmed, safety_status,
                    &safety_denied);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];

                if (!emaster_cia_process_image_update_output(
                        &session->plan->axes[axis_index], &session->images[axis_index],
                        session->controller_outputs[axis_index].control_word,
                        emaster_soem_axis_target_value(&session->plan->axes[axis_index],
                                                       axis_result),
                        session->context.slavelist[axis_index + 1U].outputs,
                        session->context.slavelist[axis_index + 1U].Obytes)) {
                    status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
                    return status;
                }
            }
            if (!safety_denied && all_axes_enabled && all_modes_confirmed)
            {
                emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_RUNNING,
                                               EMASTER_CONTROL_SESSION_OK);
            }
            (void)emaster_soem_session_publish_feedback(session, safety_status);

            /* 处理实时命令（非阻塞） */
            if (session->command_server != NULL) {
                emaster_command_t command;
                if (emaster_command_server_receive(session->command_server, &command)) {
                    emaster_command_response_t response;
                    response.success = false;

                    switch (command.type) {
                        case EMASTER_COMMAND_QUERY_STATUS: {
                            /*
                             * 状态回读只报告本周期已解码的 PDO 内容，不回读 SDO：
                             * 命令在周期线程处理，任何邮箱访问都会挤占周期预算。
                             * planned 用于区分「主站写入的目标」和「PDO 实际目标」，
                             * 两者不一致说明输出映射有问题而不是驱动器不动。
                             */
                            int offset = snprintf(response.message, sizeof(response.message),
                                "state=%d cycle=%lu axes=%zu enabled=%d completed=%d",
                                (int)session->report->state,
                                (unsigned long)session->report->cycle_count,
                                session->plan->axis_count,
                                session->report->all_axes_enabled_reached ? 1 : 0,
                                session->report->motion_completed ? 1 : 0);

                            for (axis_index = 0U;
                                 axis_index < session->plan->axis_count; ++axis_index) {
                                const emaster_control_session_axis_result_t *axis =
                                    &session->axes[axis_index];
                                int written;

                                if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                    break;
                                }
                                written = snprintf(response.message + offset,
                                                   sizeof(response.message) - (size_t)offset,
                                                   "|a%u:pos=%d,vel=%d,torque=%d,status=0x%04x,"
                                                   "target_pos=%d,planned=%d,err=0x%04x,state=%d",
                                                   (unsigned int)(axis_index + 1U),
                                                   axis->actual_position, axis->actual_velocity,
                                                   (int)axis->actual_torque,
                                                   (unsigned int)axis->status_word,
                                                   axis->target_position,
                                                   session->target_positions[axis_index],
                                                   (unsigned int)axis->drive_diagnostic.cia402_error_code,
                                                   (int)axis->cia402_state);
                                if (written < 0) {
                                    break;
                                }
                                offset += written;
                            }
                            /* 轴数多到填满缓冲区时显式标注，避免客户端把截断当完整。 */
                            if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                (void)snprintf(response.message + sizeof(response.message) - 8U, 8U,
                                              "|TRUNC");
                            }
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_QUERY_TOPOLOGY: {
                            /*
                             * 拓扑查询：返回实际轴数、总线位置和关键参数
                             * 格式: axes=N|a1:bus=B,enc=E,gear=G1/G2,torque=T|a2:...
                             */
                            int offset = snprintf(response.message, sizeof(response.message),
                                "axes=%zu", session->plan->axis_count);

                            for (axis_index = 0U;
                                 axis_index < session->plan->axis_count; ++axis_index) {
                                const emaster_control_session_axis_result_t *axis =
                                    &session->axes[axis_index];
                                const emaster_position_scale_t *scale = &axis->position_scale;
                                int written;

                                if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                    break;
                                }
                                /* 编码器分辨率 = encoder_increments / encoder_motor_revolutions
                                 * 齿轮比 = gear_motor_revolutions : gear_shaft_revolutions
                                 * 注意：rated_torque 字段可能不在 position_scale 中，暂时输出0 */
                                written = snprintf(response.message + offset,
                                                   sizeof(response.message) - (size_t)offset,
                                                   "|a%u:bus=%u,enc=%u,gear=%u/%u,torque=0",
                                                   (unsigned int)(axis_index + 1U),
                                                   (unsigned int)axis->position,
                                                   (unsigned int)scale->encoder_increments,
                                                   (unsigned int)scale->gear_motor_revolutions,
                                                   (unsigned int)scale->gear_shaft_revolutions);
                                if (written < 0) {
                                    break;
                                }
                                offset += written;
                            }
                            if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                (void)snprintf(response.message + sizeof(response.message) - 8U, 8U,
                                              "|TRUNC");
                            }
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_SWITCH_MOTION: {
                            const emaster_motion_profile_t *new_profile;
                            emaster_control_session_status_t switch_status;

                            /* payload 应该是运动配置 ID */
                            if (command.payload[0] == '\0') {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "missing motion profile ID");
                                response.success = false;
                                break;
                            }

                            new_profile = emaster_motion_profile_by_id(command.payload);
                            if (new_profile == NULL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "motion profile not found: %s", command.payload);
                                response.success = false;
                                break;
                            }

                            switch_status = emaster_soem_session_switch_motion(session, new_profile);
                            if (switch_status != EMASTER_CONTROL_SESSION_OK) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "motion switch failed: status=%d", (int)switch_status);
                                response.success = false;
                                break;
                            }

                            (void)snprintf(response.message, sizeof(response.message),
                                "motion switched to %s", command.payload);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_STOP_MOTION:
                            session->report->stop_requested = true;
                            (void)snprintf(response.message, sizeof(response.message),
                                "stop requested");
                            response.success = true;
                            break;

                        case EMASTER_COMMAND_SHUTDOWN:
                            session->report->stop_requested = true;
                            (void)snprintf(response.message, sizeof(response.message),
                                "shutdown requested");
                            response.success = true;
                            break;

                        case EMASTER_COMMAND_QUICK_STOP: {
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_set_goal(
                                    &session->controllers[axis_idx],
                                    EMASTER_CIA402_GOAL_QUICK_STOP);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "quick stop requested for %zu axes",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_HALT: {
                            bool activate = (command.payload[0] == '1');
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_set_halt(
                                    &session->controllers[axis_idx], activate);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "halt %s for %zu axes",
                                activate ? "set" : "cleared",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_FAULT_RESET: {
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_request_fault_reset(
                                    &session->controllers[axis_idx]);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "fault reset requested for %zu axes",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_SET_EXTERNAL_TARGET: {
                            /* 外部位置目标命令：解析并更新注入的目标缓冲区 */
                            emaster_external_target_buffer_t *buf = session->external_target_buffer;

                            char *token;
                            char *saveptr;
                            char payload_copy[256];
                            size_t count = 0;
                            int32_t positions[EMASTER_EXTERNAL_TARGET_MAX_AXES];

                            if (buf == NULL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "ERROR|No external target buffer configured");
                                response.success = false;
                                break;
                            }

                            strncpy(payload_copy, command.payload, sizeof(payload_copy) - 1);
                            payload_copy[sizeof(payload_copy) - 1] = '\0';

                            token = strtok_r(payload_copy, " \t", &saveptr);
                            while (token != NULL && count < EMASTER_EXTERNAL_TARGET_MAX_AXES) {
                                char *endptr;
                                long val = strtol(token, &endptr, 10);
                                /* 检查转换错误：非空字符串、完全转换、范围有效 */
                                if (*token == '\0' || *endptr != '\0' || val < INT32_MIN || val > INT32_MAX) {
                                    (void)snprintf(response.message, sizeof(response.message),
                                        "ERROR|Invalid position value: %s", token);
                                    response.success = false;
                                    goto send_response;
                                }
                                positions[count] = (int32_t)val;
                                count++;
                                token = strtok_r(NULL, " \t", &saveptr);
                            }

                            if (count == buf->axis_count && count > 0) {
                                struct timespec ts_cmd;
                                uint64_t cmd_time_ns = 0U;
                                if (clock_gettime(CLOCK_MONOTONIC, &ts_cmd) == 0) {
                                    cmd_time_ns = (uint64_t)ts_cmd.tv_sec * UINT64_C(1000000000) +
                                                  (uint64_t)ts_cmd.tv_nsec;
                                }
                                pthread_mutex_lock(&buf->mutex);
                                memcpy(buf->positions, positions, count * sizeof(int32_t));
                                buf->available = 1;
                                buf->last_update_ns = cmd_time_ns;
                                pthread_mutex_unlock(&buf->mutex);
                                (void)snprintf(response.message, sizeof(response.message),
                                    "OK|Updated %zu external targets", count);
                                response.success = true;
                            } else {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "ERROR|Wrong count: expected %zu, got %zu", buf->axis_count, count);
                                response.success = false;
                            }
                            break;
                        }

                        default:
                            (void)snprintf(response.message, sizeof(response.message),
                                "unknown command type");
                            response.success = false;
                            break;
                    }

                send_response:
                    (void)emaster_command_server_respond(session->command_server, &response);
                }
            }

            // 只有配置了运动配置时，motion_completed 才导致退出
            // 无运动配置时保持运行，等待外部实时指令
            if (session->plan->motion_profile != NULL && session->report->motion_completed) {
                break;
            }
            if (safety_denied &&
                (safety_status != EMASTER_CONTROL_SESSION_OK ||
                 session->report->stop_requested))
            {
                emaster_soem_session_set_state(
                    session,
                    safety_status == EMASTER_CONTROL_SESSION_OK
                        ? EMASTER_CONTROL_STATE_STOPPING
                        : EMASTER_CONTROL_STATE_FAULTED,
                    safety_status);
                return emaster_soem_session_publish_feedback(session, safety_status);
            }
        }
    }
    return status;
}
