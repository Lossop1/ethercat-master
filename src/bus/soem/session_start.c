#include "session_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool any_slave_state_error(const ecx_contextt *context) {
    int slave;

    if (context == NULL) {
        return true;
    }
    for (slave = 1; slave <= context->slavecount; ++slave) {
        if ((context->slavelist[slave].state & EC_STATE_ERROR) != 0U) {
            return true;
        }
    }
    return false;
}

/*
 * 从站确认进入 OP 后，使用最后一次 OP 过程数据建立保持目标和相对运动起点
 * SAFE-OP 位置只保留为观察值，不参与目标计算
 */
static emaster_control_session_status_t prepare_op_position(
    emaster_soem_session_t *session)
{
    size_t axis_index;

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        if (!emaster_cia_process_image_decode_input(
                &session->images[axis_index], slave->inputs, slave->Ibytes,
                &mode_display, &status_word, &actual_position))
        {
            return EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
        }
        axis->input_decoded = true;
        axis->mode_display = mode_display;
        axis->status_word = status_word;
        axis->initial_actual_position = actual_position;
        (void)emaster_soem_axis_set_feedback(&session->plan->axes[axis_index], axis,
                                              actual_position);
        axis->target_position = session->plan->motion_profile != NULL &&
                                        session->plan->motion_profile->trajectory ==
                                            EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY
                                    ? 0
                                    : actual_position;
        session->status_words[axis_index] = status_word;
        if (session->plan->motion_profile != NULL)
        {
            session->actual_positions[axis_index] = actual_position;
            session->target_positions[axis_index] = session->axes[axis_index].target_position;
        }
        if (!emaster_cia_process_image_update_output(
                &session->plan->axes[axis_index], &session->images[axis_index],
                UINT16_C(0), axis->target_position, slave->outputs, slave->Obytes))
        {
            return EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        }
    }
    if (session->plan->motion_profile != NULL)
    {
        if (session->plan->motion_profile->trajectory ==
            EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION)
        {
            emaster_relative_motion_status_t motion_status = emaster_relative_motion_init(
                session->plan->motion_profile, session->motion_axis_configs,
                session->motion_scales, session->target_positions,
                session->plan->axis_count, session->plan->cycle_ns,
                session->motion_axes, &session->motion);
            if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE)
                return EMASTER_CONTROL_SESSION_MOTION_INVALID;
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
            {
                if (!emaster_relative_motion_axis_within_software_limits(
                        &session->motion_axes[axis_index],
                        session->axes[axis_index].software_position_limit_min,
                        session->axes[axis_index].software_position_limit_max))
                    return EMASTER_CONTROL_SESSION_MOTION_INVALID;
                session->axes[axis_index].motion_final_position =
                    session->motion_axes[axis_index].final_position;
                session->axes[axis_index].max_following_error_counts =
                    session->motion_axes[axis_index].max_following_error_counts;
            }
            session->motion_prepared = true;
        }
        else if (session->plan->motion_profile->trajectory ==
                 EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY)
        {
            if (!emaster_velocity_motion_init(
                    session->plan->motion_profile, session->motion_axis_configs,
                    session->motion_scales, session->plan->axis_count,
                    session->plan->cycle_ns, session->velocity_axes,
                    &session->velocity_motion))
                return EMASTER_CONTROL_SESSION_MOTION_INVALID;
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
                session->axes[axis_index].max_following_error_counts =
                    session->velocity_axes[axis_index].max_velocity_error;
            session->velocity_motion_prepared = true;
        }
    }
    return EMASTER_CONTROL_SESSION_OK;
}

emaster_control_session_status_t emaster_soem_session_start(emaster_soem_session_t *session) {
    size_t axis_index;
    emaster_control_session_status_t status;

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];
        ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

        session->axes[axis_index].output_initialized = emaster_cia_process_image_prepare_output(
            axis, &session->images[axis_index], slave->outputs, slave->Obytes);
        if (!session->axes[axis_index].output_initialized) {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            return status;
        }
    }
    if (!emaster_session_observer_prepare_audit(
            session->plan, session->images, session->transition_cycles, &session->report->audit)) {
        status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
        return status;
    }
    session->report->expected_wkc = (uint16_t)(session->context.grouplist[0].outputsWKC * 2U +
                                               session->context.grouplist[0].inputsWKC);
    if (!emaster_cycle_clock_init(&session->clock, session->plan->cycle_ns,
                                  session->plan->process_data_phase_ns)) {
        status = EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;
        return status;
    }
    session->cycle_output_active = true;
    session->report->process_data_phase_ns = session->plan->process_data_phase_ns;
    session->report->dc_startup_cycles_requested = session->plan->dc_startup_cycles;

    /* SAFE-OP 首次周期反馈用于锁定 CSP 当前实际位置，控制字仍保持为零。 */
    status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION);
    if (status != EMASTER_CONTROL_SESSION_OK) {
        return status;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        session->axes[axis_index].input_decoded = emaster_cia_process_image_decode_input(
            &session->images[axis_index], slave->inputs, slave->Ibytes, &mode_display, &status_word,
            &actual_position);
        if (!session->axes[axis_index].input_decoded) {
            status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
            return status;
        }
        if (!emaster_cia_process_image_audit_input(
                &session->images[axis_index], &session->report->audit,
                EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION, (uint16_t)(axis_index + 1U),
                session->exchange)) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
        session->axes[axis_index].requested_mode =
            session->plan->axes[axis_index].operation_mode->value;
        if (!session->images[axis_index].tx_mode_available)
        {
            /* 固定 PDO 没有 6061 字段，复用 SAFE-OP SDO 读回，避免 OP 中再次访问邮箱。 */
            session->axes[axis_index].mode_display_sdo_read =
                session->axes[axis_index].safeop_mode_display_sdo_read;
            session->axes[axis_index].mode_display_sdo =
                session->axes[axis_index].safeop_mode_display_sdo;
            session->axes[axis_index].mode_display =
                session->axes[axis_index].mode_display_sdo;
        }
        if (session->images[axis_index].tx_mode_available)
        {
            session->axes[axis_index].mode_display = mode_display;
        }
        session->axes[axis_index].status_word = status_word;
        session->axes[axis_index].safeop_actual_position = actual_position;
        (void)emaster_soem_axis_set_feedback(&session->plan->axes[axis_index],
                                              &session->axes[axis_index], actual_position);
        session->axes[axis_index].target_position =
            session->plan->axes[axis_index].operation_mode->value == INT8_C(9)
                ? 0 : actual_position;
        if (session->plan->motion_profile != NULL) {
            session->actual_positions[axis_index] = actual_position;
            session->target_positions[axis_index] = session->axes[axis_index].target_position;
        }
        if (!emaster_cia_process_image_update_output(
                &session->plan->axes[axis_index], &session->images[axis_index], UINT16_C(0),
                session->axes[axis_index].target_position,
                session->context.slavelist[axis_index + 1U].outputs,
                session->context.slavelist[axis_index + 1U].Obytes)) {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            return status;
        }
    }
    /*
     * SAFE-OP 的 6064 只作为现场观察值，不在这里建立运动起点
     * 驱动器可能在切入 OP 时完成位置初始化，运动起点必须来自 OP 首个有效反馈
     */
    session->motion_prepared = false;
    if (session->dc_required) {
        uint64_t startup_cycle;
        uint64_t max_startup_exchanges;
        uint32_t stable_cycles = 0U;

        if ((uint64_t)session->plan->dc_startup_cycles >
            (UINT64_MAX - session->transition_cycles) / UINT64_C(2))
        {
            return EMASTER_CONTROL_SESSION_DC_SYNC_FAILED;
        }
        max_startup_exchanges =
            (uint64_t)session->plan->dc_startup_cycles * UINT64_C(2) +
            session->transition_cycles;

        session->report->dc_startup_cycles_completed = 0U;
        for (startup_cycle = 0U; startup_cycle < max_startup_exchanges;
             ++startup_cycle) {
            bool sample_safe = true;

            status =
                emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return status;
            }
            ++session->report->dc_startup_exchanges;
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
            {
                if (session->plan->axes[axis_index].operation_profile->sync_strategy ==
                        EMASTER_SYNC_STRATEGY_DC &&
                    !emaster_cyclic_timing_last_sample_is_safe(
                        &session->axes[axis_index].timing))
                {
                    sample_safe = false;
                    break;
                }
            }
            if (sample_safe)
            {
                ++stable_cycles;
            }
            else
            {
                stable_cycles = 0U;
            }
            session->report->dc_startup_cycles_completed = stable_cycles;
            if (stable_cycles >= session->plan->dc_startup_cycles)
            {
                break;
            }
            if (session->stop_requested != NULL &&
                session->stop_requested(session->stop_user_data)) {
                session->report->stop_requested = true;
                return status;
            }
        }
        if (!session->clock.dc_feedback_valid ||
            stable_cycles < session->plan->dc_startup_cycles) {
            status = EMASTER_CONTROL_SESSION_DC_SYNC_FAILED;
            return status;
        }
        session->report->dc_startup_stable = true;
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            if (session->plan->axes[axis_index].operation_profile->sync_strategy ==
                    EMASTER_SYNC_STRATEGY_DC &&
                !emaster_cyclic_timing_last_sample_is_safe(
                    &session->axes[axis_index].timing)) {
                session->report->dc_startup_stable = false;
                status = EMASTER_CONTROL_SESSION_DC_SYNC_FAILED;
                return status;
            }
        }
        session->report->dc_startup_phase_error_ns =
            emaster_cycle_clock_phase_error_ns(&session->clock);
    }

    {
        emaster_safety_conditions_t conditions;
        emaster_safety_decision_t decision;

        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            emaster_control_session_axis_result_t *axis = &session->axes[axis_index];

            axis->mode_display_match = axis->mode_display == axis->requested_mode;
        }
        memset(&conditions, 0, sizeof(conditions));
        conditions.topology_verified = true;
        conditions.pdo_verified = true;
        conditions.output_initialized = true;
        conditions.communication_healthy = true;
        conditions.command_valid = session->plan->motion_profile == NULL ||
                                   session->plan->motion_profile->approval ==
                                       EMASTER_MOTION_PROFILE_APPROVED;
        conditions.feedback_valid = true;
        /* 动态 PDO 的 6061 在进入 OP 后确认；固定 PDO 以 SAFE-OP 的 6060 写后读回作为前提 */
        conditions.mode_confirmed = true;
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
        {
            if (!session->images[axis_index].tx_mode_available)
            {
                const emaster_control_session_axis_result_t *axis =
                    &session->axes[axis_index];

                conditions.mode_confirmed &= axis->mode_command_sdo_read &&
                                             axis->mode_command_sdo == axis->requested_mode;
            }
        }
        conditions.synchronization_healthy = !session->dc_required ||
                                             session->report->dc_startup_stable;
        conditions.target_valid = session->plan->motion_profile == NULL ||
                                  session->motion_prepared;
        conditions.drive_limit_inactive = true;
        conditions.enable_authorized = true;
        if (!emaster_safety_evaluate(&conditions, &decision)) {
            return EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
        }
        session->report->safety_blocking_reasons = decision.blocking_reasons;
        session->report->safety_control_permitted = decision.control_permitted;
    }

    /*
     * OP 请求期间交替交换已锁定目标的 PDO 和读取 AL 状态。状态读取的耗时仍计入
     * 同一周期预算，超时由周期时钟报告，不能把状态检查成功等同于周期时序正确。
     */
    session->context.slavelist[0].state = EC_STATE_OPERATIONAL;
    if (ecx_writestate(&session->context, 0U) <= 0) {
        status = EMASTER_CONTROL_SESSION_OP_NOT_REACHED;
        return status;
    }
    for (uint64_t transition_cycle = 0U; transition_cycle < session->op_transition_cycles;
         ++transition_cycle) {
        int bus_state;

        status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_OPERATION_REQUEST);
        if (status != EMASTER_CONTROL_SESSION_OK) {
            return status;
        }
        bus_state = ecx_readstate(&session->context);
        if (bus_state == EC_STATE_OPERATIONAL && !any_slave_state_error(&session->context)) {
            session->report->op_reached = true;
            break;
        }
        if (any_slave_state_error(&session->context)) {
            break;
        }
    }
    if (!session->report->op_reached) {
        ecx_readstate(&session->context);
        status = EMASTER_CONTROL_SESSION_OP_NOT_REACHED;
        return status;
    }
    status = prepare_op_position(session);
    if (status != EMASTER_CONTROL_SESSION_OK)
    {
        return status;
    }
    emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_OPERATIONAL,
                                   EMASTER_CONTROL_SESSION_OK);
    /*
     * 进入 OP 后必须持续发送周期过程数据。同步 SDO 会占用多个周期，可能使
     * DC-Sync0 从站判定 SM2 输出事件丢失；模式显示直接使用已映射的 TxPDO，
     * 完整 SDO 诊断放在安全停机之后执行。
     */
    return EMASTER_CONTROL_SESSION_OK;
}
