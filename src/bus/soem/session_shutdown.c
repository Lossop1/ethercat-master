#include "session_internal.h"

#include <limits.h>
#include <stdio.h>
#include <time.h>

/*
 * 停机路径上的 AL 快照。停机结束后读到的 AL 状态（SAFE-OP + 0x1A）只能说明
 * 退出时驱动器不在 OP，无法区分三种来源：周期运行中掉出、停机序言阻塞期间掉出、
 * 或者被安全停机帧打掉。在两个时刻各读一次，把时间坐标补上。
 *
 * 同时写进报告：判据必须留在报告里，否则"序言阻塞导致掉出 OP"这条结论只能靠
 * 日志复述，报告本身仍然只有停机后的一个坐标。
 */
static void snapshot_al_states(emaster_soem_session_t *session, const char *label,
                               bool at_entry)
{
    size_t axis_index;

    if (session == NULL || session->plan == NULL)
    {
        return;
    }
    ecx_readstate(&session->context);
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

        printf("[SHUTDOWN] %s 轴%zu: AL state=%u, status_code=0x%04X\n", label,
               axis_index + 1U, (unsigned int)slave->state,
               (unsigned int)slave->ALstatuscode);
        if (at_entry)
        {
            session->axes[axis_index].shutdown_entry_al_state = slave->state;
            session->axes[axis_index].shutdown_entry_al_status_code = slave->ALstatuscode;
        }
        else
        {
            session->axes[axis_index].shutdown_pre_stop_al_state = slave->state;
            session->axes[axis_index].shutdown_pre_stop_al_status_code = slave->ALstatuscode;
        }
    }
}

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

/*
 * 停机仍使用相同的控制器和过程映像；任何一次交换失败都不能当作停用确认。
 *
 * 这里刻意不经过多轴协调器。协调器会重新解码状态字，解不出来就拒绝整帧并返回
 * 错误——而"状态字解不出来"恰恰是最需要发出停用命令的情形。早退会让安全输出一次
 * 都发不出去：现场表现是报告里 safe_output_sent=false，而主站的失败消息却声称
 * "已尝试发送安全输出"。目标在进入本函数前已固定为 SAFE_STOP，控制器对未知状态
 * 只会产生 Disable Voltage（0x0000），逐轴直接计算不会产生任何使能位；跨轴一致性
 * 在此也没有意义，每轴独立撤销使能就是想要的终点。
 *
 * 被放弃的只有协调器的两项检查：序号单调和帧截止时间。两者都是为"继续驱动轴"
 * 服务的，在撤销使能的路径上没有对应风险。
 */
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
        bool all_axes_safe = true;
        uint64_t exchange_before = session->exchange;

        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

            /* 失败时 controller_outputs 保持上周期值，此时直接返回不发帧。 */
            if (!emaster_cia402_controller_step(&session->controllers[axis_index],
                                                session->status_words[axis_index],
                                                &session->controller_outputs[axis_index])) {
                return false;
            }
            session->axes[axis_index].control_word =
                session->controller_outputs[axis_index].control_word;
            if (!emaster_cia_process_image_update_output(
                    &session->plan->axes[axis_index], &session->images[axis_index],
                    session->controller_outputs[axis_index].control_word,
                    emaster_soem_axis_target_value(&session->plan->axes[axis_index],
                                                   &session->axes[axis_index]),
                    slave->outputs, slave->Obytes)) {
                return false;
            }
        }
        if (emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFE_STOP) !=
            EMASTER_CONTROL_SESSION_OK) {
            return false;
        }
        /*
         * 只有真正发出过帧才算"已发送安全输出"。周期时钟恢复会把本周期整帧跳过
         * （session_exchange.c 的 deadline_recovery 分支），那时交换号不变，不算数。
         */
        if (session->exchange > exchange_before) {
            session->report->safe_output_sent = true;
            /*
             * 序言缺口只记一次：起点是停机入口抓的最后一条周期帧发送时刻，终点是这里——
             * 第一条真正发出去的安全停机帧。两者同为 CLOCK_MONOTONIC 的发送结束时刻，
             * 差值就是驱动器在这段停机序言里没收到任何过程数据的窗口。
             */
            if (session->report->shutdown_prologue_gap_ns == 0U &&
                session->shutdown_prologue_start_ns != 0U) {
                uint64_t send_end_ns = session->axes[0].timing.last_host_send_end_ns;

                if (send_end_ns > session->shutdown_prologue_start_ns) {
                    session->report->shutdown_prologue_gap_ns =
                        send_end_ns - session->shutdown_prologue_start_ns;
                }
            }
        }
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
            emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
            emaster_cia402_state_t state;
            int32_t actual_position = 0;

            axis->input_decoded = emaster_cia_process_image_decode_input(
                &session->images[axis_index], slave->inputs, slave->Ibytes, &axis->mode_display,
                &axis->status_word, &actual_position);
            (void)emaster_soem_axis_set_feedback(&session->plan->axes[axis_index], axis,
                                                  actual_position);
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
            {
                const emaster_slave_profile_t *profile =
                    session->plan->axes[axis_index].device_profile;
                bool device_safe = profile != NULL && profile->safe_stop_status_mask != 0U &&
                                   (axis->status_word & profile->safe_stop_status_mask) ==
                                       profile->safe_stop_status_value;
                bool standard_safe = state == EMASTER_CIA402_STATE_SWITCH_ON_DISABLED ||
                                     state == EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON;

                if (!device_safe && !standard_safe) {
                    all_axes_safe = false;
                }
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

    /*
     * 缺口测量的起点：最后一条周期帧的发送结束时刻。此刻 timing 里的最后一条样本就是
     * 周期回路的最后一条帧，安全停机帧还没发（下面的 join 与快照都在窗口之内）。
     */
    if (session->plan != NULL && session->plan->axis_count > 0U)
    {
        session->shutdown_prologue_start_ns = session->axes[0].timing.last_host_send_end_ns;
    }

    /* 进入停机时的 AL 快照：此时周期回路已经退出，但一个停机帧都还没发。 */
    if (session->context_open)
    {
        snapshot_al_states(session, "停机入口", true);
    }

    /* P4.3: 停止 SDO 慢速观测线程。这是停机序言里唯一的阻塞点，计时留证。 */
    {
        struct timespec join_start;
        struct timespec join_end;

        (void)clock_gettime(CLOCK_MONOTONIC, &join_start);
        emaster_soem_session_stop_observer(session);
        if (clock_gettime(CLOCK_MONOTONIC, &join_end) == 0)
        {
            uint64_t start_ns = (uint64_t)join_start.tv_sec * UINT64_C(1000000000) +
                                (uint64_t)join_start.tv_nsec;
            uint64_t end_ns = (uint64_t)join_end.tv_sec * UINT64_C(1000000000) +
                              (uint64_t)join_end.tv_nsec;

            session->report->shutdown_observer_join_ns = end_ns - start_ns;
            printf("[SHUTDOWN] 停止观测线程耗时 %.3f ms（该窗口内不发送过程数据）\n",
                   (double)(end_ns - start_ns) / 1000000.0);
        }
    }

    if (!session->context_open) {
        return;
    }
    /*
     * 安全停机帧之前再读一次。两次快照相同 → 驱动器是在周期运行期间掉出 OP；
     * 第一次在 OP、第二次掉出 → 掉出发生在停机序言（观测线程收尾等）阻塞期间，
     * 与安全停机帧无关。这正是 WKC 从 9 掉到 3 的两种互斥解释。
     */
    snapshot_al_states(session, "安全停机前", false);
    /*
     * 审计在周期阶段是封顶的（超出预算的样本只计数不保存）。若在这里才解封，
     * 则安全停机阶段——恰恰是故障发生的阶段——一个样本都留不下。
     */
    emaster_run_audit_end_cyclic(&session->report->audit);
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
            if (session->report->shutdown_prologue_gap_ns != 0U)
            {
                printf("[SHUTDOWN] 停机序言过程数据缺口 %.3f ms"
                       "（最后一条周期帧 → 第一条安全停机帧）\n",
                       (double)session->report->shutdown_prologue_gap_ns / 1000000.0);
            }
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
    /*
     * 停机诊断此前只在 PRE-OP 到达后执行。驱动器掉出 OP（SAFE-OP + 0x1A）会让
     * PRE-OP 切换超时，于是"最需要诊断的那次运行"里，全部停机 SDO 诊断一起被跳过：
     * 报告中的 read_succeeded=false 分不清"没尝试"和"读失败"，SM 同步计数器
     * （1C32/1C33）就是这样一轮都没读到。邮箱在 SAFE-OP 下仍然可用，会话还在就能读。
     */
    if (session->report->diagnostic_preop_reached || session->report->safe_op_reached) {
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

    /* 销毁实时命令服务器 */
    if (session->command_server != NULL) {
        emaster_command_server_destroy(session->command_server);
        session->command_server = NULL;
    }
}
