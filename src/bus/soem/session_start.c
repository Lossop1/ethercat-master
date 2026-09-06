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
        session->axes[axis_index].initial_actual_position = actual_position;
        session->axes[axis_index].actual_position = actual_position;
        session->axes[axis_index].target_position = actual_position;
        if (session->plan->motion_profile != NULL) {
            session->actual_positions[axis_index] = actual_position;
            session->target_positions[axis_index] = actual_position;
        }
        if (!emaster_cia_process_image_update_output(
                &session->plan->axes[axis_index], &session->images[axis_index], UINT16_C(0),
                actual_position, session->context.slavelist[axis_index + 1U].outputs,
                session->context.slavelist[axis_index + 1U].Obytes)) {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            return status;
        }
    }
    if (session->dc_required) {
        uint32_t startup_cycle;

        session->report->dc_startup_cycles_completed = 1U;
        for (startup_cycle = 1U; startup_cycle < session->plan->dc_startup_cycles;
             ++startup_cycle) {
            status =
                emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return status;
            }
            session->report->dc_startup_cycles_completed = startup_cycle + 1U;
            if (session->stop_requested != NULL &&
                session->stop_requested(session->stop_user_data)) {
                session->report->stop_requested = true;
                return status;
            }
        }
        if (!session->clock.dc_feedback_valid) {
            status = EMASTER_CONTROL_SESSION_DC_SYNC_FAILED;
            return status;
        }
        session->report->dc_startup_phase_error_ns =
            emaster_cycle_clock_phase_error_ns(&session->clock);
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
    /*
     * 进入 OP 后必须持续发送周期过程数据。同步 SDO 会占用多个周期，可能使
     * DC-Sync0 从站判定 SM2 输出事件丢失；模式显示直接使用已映射的 TxPDO，
     * 完整 SDO 诊断放在安全停机之后执行。
     */
    return EMASTER_CONTROL_SESSION_OK;
}
