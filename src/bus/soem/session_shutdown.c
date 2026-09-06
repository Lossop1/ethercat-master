#include "session_internal.h"

#include <limits.h>

static void disable_sync0(ecx_contextt *context, emaster_cia_process_image_t *runtime,
                          size_t count) {
    size_t axis_index;

    if (context == NULL || runtime == NULL) {
        return;
    }
    for (axis_index = 0U; axis_index < count; ++axis_index) {
        if (runtime[axis_index].sync0_configured) {
            ecx_dcsync0(context, (uint16_t)(axis_index + 1U), FALSE, 0U, 0);
        }
    }
}

/* 停机仍使用相同的控制器和过程映像；任何一次交换失败都不能当作停用确认。 */
static bool stop_process_data(emaster_soem_session_t *session) {
    uint64_t timeout_ns;
    uint64_t max_cycles;
    uint64_t cycle_index;
    size_t axis_index;

    timeout_ns = (uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000);
    max_cycles = (timeout_ns + session->plan->cycle_ns - UINT64_C(1)) / session->plan->cycle_ns;
    if (max_cycles == 0U) {
        max_cycles = 1U;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        session->status_words[axis_index] = session->axes[axis_index].status_word;
        if (!emaster_cia402_controller_set_goal(&session->controllers[axis_index],
                                                EMASTER_CIA402_GOAL_SAFE_STOP)) {
            return false;
        }
    }

    for (cycle_index = 0U; cycle_index < max_cycles; ++cycle_index) {
        emaster_multiaxis_frame_t frame;
        bool all_axes_safe = true;
        uint64_t now_ns;
        uint64_t deadline_ns;

        if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &deadline_ns)) {
            return false;
        }
        frame.sequence = session->report->cycle_count + UINT64_C(1);
        frame.deadline_ns = deadline_ns;
        frame.axis_count = session->plan->axis_count;
        frame.status_words = session->status_words;
        frame.outputs = session->controller_outputs;
        if (emaster_multiaxis_coordinator_step(&session->coordinator, &frame, now_ns) !=
            EMASTER_MULTIAXIS_OK) {
            return false;
        }
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

            session->axes[axis_index].control_word =
                session->controller_outputs[axis_index].control_word;
            if (!emaster_cia_process_image_update_output(
                    &session->plan->axes[axis_index], &session->images[axis_index],
                    session->controller_outputs[axis_index].control_word,
                    session->axes[axis_index].target_position, slave->outputs, slave->Obytes)) {
                return false;
            }
        }
        if (emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFE_STOP) !=
            EMASTER_CONTROL_SESSION_OK) {
            return false;
        }
        session->report->safe_output_sent = true;
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
            emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
            emaster_cia402_state_t state;
            int32_t actual_position = 0;

            axis->input_decoded = emaster_cia_process_image_decode_input(
                &session->images[axis_index], slave->inputs, slave->Ibytes, &axis->mode_display,
                &axis->status_word, &actual_position);
            axis->actual_position = actual_position;
            session->status_words[axis_index] = axis->status_word;
            if (axis->input_decoded) {
                (void)emaster_cia_process_image_audit_input(
                    &session->images[axis_index], &session->report->audit,
                    EMASTER_AUDIT_PHASE_SAFE_STOP, (uint16_t)(axis_index + 1U), session->exchange);
            }
            if (!axis->input_decoded ||
                !emaster_cia402_decode_status_word(axis->status_word, &state)) {
                all_axes_safe = false;
                continue;
            }
            axis->cia402_state = state;
            if (state != EMASTER_CIA402_STATE_SWITCH_ON_DISABLED &&
                state != EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON) {
                all_axes_safe = false;
            }
        }
        if (all_axes_safe) {
            return true;
        }
    }
    return false;
}

void emaster_soem_session_shutdown(emaster_soem_session_t *session) {
    size_t axis_index;

    if (!session->context_open) {
        return;
    }
    if (session->process_map_ready && session->cycle_output_active) {
        bool communication_usable =
            session->report->status != EMASTER_CONTROL_SESSION_WKC_MISMATCH &&
            session->report->status != EMASTER_CONTROL_SESSION_DC_SYNC_FAILED &&
            session->report->status != EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED &&
            session->report->status != EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;

        if (session->report->status == EMASTER_CONTROL_SESSION_OK)
        {
            emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_STOPPING,
                                           EMASTER_CONTROL_SESSION_OK);
        }
        /*
         * 只有通信和周期时钟仍可用时，才能把逐级停用称为已确认
         * WKC、DC 或截止时间失效后继续发送只能算尽力而为，不能伪造安全到达结论
         */
        if (communication_usable) {
            session->report->safe_state_reached = stop_process_data(session);
        }
        if (communication_usable && !session->report->safe_state_reached &&
            session->report->status == EMASTER_CONTROL_SESSION_OK) {
            session->report->status = EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED;
            session->report->fault_latched = true;
            emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                           session->report->status);
        }
    }

    /* 先结束输出应用状态，再执行同步邮箱读取；诊断不再制造运行中的周期空洞。 */
    if (session->report->op_reached) {
        ecx_readstate(&session->context);
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            session->axes[axis_index].shutdown_al_state =
                session->context.slavelist[axis_index + 1U].state;
            session->axes[axis_index].shutdown_al_status_code =
                session->context.slavelist[axis_index + 1U].ALstatuscode;
        }
        session->context.slavelist[0].state = EC_STATE_PRE_OP;
        session->report->diagnostic_preop_reached =
            ecx_writestate(&session->context, 0U) > 0 &&
            ecx_statecheck(&session->context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE) ==
                EC_STATE_PRE_OP;
    }
    if (session->sync0_started) {
        disable_sync0(&session->context, session->images, session->plan->axis_count);
        session->report->sync0_disabled = true;
    }
    emaster_run_audit_end_cyclic(&session->report->audit);
    if (session->report->diagnostic_preop_reached) {
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];
            emaster_soem_sdo_reader_context_t sdo;

            emaster_soem_sdo_context_init(&sdo, &session->context, (uint16_t)(axis_index + 1U),
                                          &session->report->audit,
                                          EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC, session->exchange);
            axis_result->mode_command_sdo_read = emaster_soem_read_i8(
                &sdo, UINT16_C(0x6060), UINT8_C(0), &axis_result->mode_command_sdo);
            axis_result->mode_display_sdo_read = emaster_soem_read_i8(
                &sdo, UINT16_C(0x6061), UINT8_C(0), &axis_result->mode_display_sdo);
            axis_result->following_error_read = emaster_soem_read_i32(
                &sdo, UINT16_C(0x60F4), UINT8_C(0),
                &axis_result->following_error_actual);
            /*
             * 这里是退出 OP 后的诊断快照。计数器可能包含停机和状态转换期间的事件，
             * 不能与 first_cycle_failure 中的首次周期异常等同。
             */
            emaster_session_observer_read_drive(&sdo, &axis_result->drive_diagnostic);
            emaster_session_observer_read_sync(&sdo, UINT16_C(0x1C32),
                                               &axis_result->sm2_diagnostic);
            emaster_session_observer_read_sync(&sdo, UINT16_C(0x1C33),
                                               &axis_result->sm3_diagnostic);
            if (!axis_result->position_scale.read_succeeded) {
                emaster_session_observer_read_position_scale(&sdo, &axis_result->position_scale);
            }
            axis_result->final_diagnostic_read_count =
                session->plan->axes[axis_index].operation_mode->final_sdo_read_count;
            for (size_t diagnostic_index = 0U;
                 diagnostic_index <
                 session->plan->axes[axis_index].operation_mode->final_sdo_read_count;
                 ++diagnostic_index) {
                if (emaster_session_observer_read_configured_sdo(
                        &sdo, &session->plan->axes[axis_index]
                                   .operation_mode->final_sdo_reads[diagnostic_index])) {
                    ++axis_result->final_diagnostic_success_count;
                }
            }
        }
    }
    session->report->last_dc_time_ns = session->context.DCtime;
    session->report->restore_init_succeeded = emaster_soem_restore_init(&session->context);
    if (!session->report->restore_init_succeeded &&
        session->report->status == EMASTER_CONTROL_SESSION_OK) {
        session->report->status = EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED;
        session->report->fault_latched = true;
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       session->report->status);
    }
    if (session->report->status == EMASTER_CONTROL_SESSION_OK &&
        session->report->safe_state_reached)
    {
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_STOPPED,
                                       EMASTER_CONTROL_SESSION_OK);
    }
    else if (session->report->status != EMASTER_CONTROL_SESSION_OK)
    {
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       session->report->status);
    }
    ecx_close(&session->context);
    session->context_open = false;
}
