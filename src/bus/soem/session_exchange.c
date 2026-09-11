#include "session_internal.h"

#include <string.h>
#include <time.h>

/*
 * WKC 错误恢复由配置策略决定，不再硬编码阈值。
 * 配置策略在 config/error_recovery_policies/ 中定义，部署配置引用具体策略。
 */

static void note_deadline_missed(emaster_soem_session_t *session)
{
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        emaster_cyclic_timing_stats_note_deadline_missed(&session->axes[axis].timing);
    }
}

/*
 * 按配置策略判断本次死区超时是否在可恢复范围内。
 * 若策略允许：重置时钟 deadline 至最近未来边界，清除 deadline_missed，返回 true。
 * 若策略不允许或时钟重置失败：返回 false，调用者负责锁存失败。
 */
static bool try_deadline_recovery(emaster_soem_session_t *session)
{
    if (session->error_recovery_policy == NULL ||
        !session->error_recovery_policy->deadline_recovery.enabled)
    {
        return false;
    }
    if (session->clock.consecutive_deadline_misses >
        session->error_recovery_policy->deadline_recovery.consecutive_error_threshold)
    {
        return false;
    }
    return emaster_cycle_clock_recover(&session->clock);
}

static emaster_control_session_status_t fail_exchange(emaster_soem_session_t *session,
                                                      emaster_audit_phase_t phase,
                                                      emaster_control_session_status_t status,
                                                      bool wkc_available) {
    emaster_cycle_failure_t *failure = &session->report->first_cycle_failure;

    if (!failure->present) {
        failure->present = true;
        failure->status = status;
        failure->phase = phase;
        failure->exchange = session->exchange;
        failure->wkc_available = wkc_available;
        failure->wkc = session->report->actual_wkc;
        failure->dc_time_ns = session->context.DCtime;
    }
    return status;
}

emaster_control_session_status_t emaster_soem_session_exchange(emaster_soem_session_t *session,
                                                               emaster_audit_phase_t phase) {
    bool matched;
    uint64_t deadline_ns;
    uint64_t now_ns;
    uint64_t next_deadline_ns;
    struct timespec send_start;
    struct timespec send_end;
    struct timespec receive_end;
    emaster_cyclic_timing_observation_t timing;

    if (!emaster_cycle_clock_wait(&session->clock)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        if (session->clock.deadline_missed)
        {
            note_deadline_missed(session);
            if (try_deadline_recovery(session))
            {
                /* 连续超次数在阈值内，重新对齐 deadline，跳过本周期交换继续运行。 */
                return EMASTER_CONTROL_SESSION_OK;
            }
            emaster_soem_session_latch_failure(
                session, EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
        }
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             false);
    }
    /* 唤醒后立即发送。审计不再位于定时点与发包之间，输出值在下一次编码前不变。 */
    if (!emaster_cycle_clock_deadline_ns(&session->clock, &deadline_ns) ||
        clock_gettime(CLOCK_MONOTONIC, &send_start) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, false);
    }
    ++session->exchange;
    (void)ecx_send_processdata(&session->context);
    if (clock_gettime(CLOCK_MONOTONIC, &send_end) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, false);
    }
    session->report->actual_wkc = ecx_receive_processdata(&session->context, EC_TIMEOUTRET);
    if (clock_gettime(CLOCK_MONOTONIC, &receive_end) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, true);
    }
    ++session->report->cycle_count;
    matched = session->report->actual_wkc == (int)session->report->expected_wkc;
    if (!matched) {
        ++session->wkc_consecutive_errors;
        ++session->wkc_total_errors;
        ++session->report->wkc_error_count;
        session->report->wkc_consecutive_errors = session->wkc_consecutive_errors;
        if (session->wkc_consecutive_errors > session->report->wkc_max_consecutive_errors) {
            session->report->wkc_max_consecutive_errors = session->wkc_consecutive_errors;
        }

        /* 检查是否超过配置的容错阈值 */
        bool should_fail = false;
        if (session->error_recovery_policy != NULL &&
            session->error_recovery_policy->wkc_recovery.enabled) {
            const emaster_wkc_recovery_config_t *wkc_cfg =
                &session->error_recovery_policy->wkc_recovery;
            if (session->wkc_consecutive_errors >= wkc_cfg->consecutive_error_threshold ||
                session->wkc_total_errors >= wkc_cfg->total_error_threshold) {
                should_fail = true;
            }
        } else {
            /* 未配置策略或WKC恢复未启用，首次错误即停机（保守默认行为） */
            should_fail = true;
        }

        if (should_fail) {
            (void)fail_exchange(session, phase, EMASTER_CONTROL_SESSION_WKC_MISMATCH, true);
        }
    } else {
        /* WKC 恢复正常，重置连续错误计数 */
        session->wkc_consecutive_errors = 0U;
        session->report->wkc_consecutive_errors = 0U;
    }
    memset(&timing, 0, sizeof(timing));
    timing.exchange = session->exchange;
    timing.host_time_valid = true;
    timing.scheduled_send_ns = deadline_ns;
    timing.host_send_start_ns =
        (uint64_t)send_start.tv_sec * UINT64_C(1000000000) + (uint64_t)send_start.tv_nsec;
    timing.host_send_end_ns =
        (uint64_t)send_end.tv_sec * UINT64_C(1000000000) + (uint64_t)send_end.tv_nsec;
    timing.host_receive_end_ns =
        (uint64_t)receive_end.tv_sec * UINT64_C(1000000000) + (uint64_t)receive_end.tv_nsec;
    timing.dc_time_valid = session->dc_required && session->context.DCtime > 0;
    timing.dc_time_ns = session->context.DCtime;
    timing.wkc_match = matched;
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        const emaster_operation_profile_t *operation =
            session->plan->axes[axis].operation_profile;
        if (!emaster_cyclic_timing_stats_record(
                &session->axes[axis].timing, &timing, session->plan->cycle_ns,
                operation->process_data_phase_ns, operation->sync0_shift_ns,
                session->context.slavelist[axis + 1U].pdelay))
        {
            return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_AUDIT_FAILED, true);
        }
    }
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis) {
        if (!emaster_cia_process_image_audit_output(&session->images[axis], &session->report->audit,
                                                    phase, (uint16_t)(axis + 1U), session->exchange,
                                                    matched)) {
            return matched
                       ? fail_exchange(session, phase, EMASTER_CONTROL_SESSION_AUDIT_FAILED, true)
                       : EMASTER_CONTROL_SESSION_WKC_MISMATCH;
        }
    }
    /*
     * WKC 容错策略：单次或少量WKC错误可以容忍，只有持续或频繁错误才停机。
     * 这样可以避免瞬态干扰导致的误停机，同时保持对严重通信故障的响应。
     */
    if (!matched) {
        if (session->error_recovery_policy != NULL &&
            (session->wkc_consecutive_errors >=
             session->error_recovery_policy->wkc_recovery.consecutive_error_threshold ||
             session->wkc_total_errors >=
             session->error_recovery_policy->wkc_recovery.total_error_threshold)) {
            emaster_soem_session_latch_failure(session,
                                                EMASTER_CONTROL_SESSION_WKC_MISMATCH);
            return EMASTER_CONTROL_SESSION_WKC_MISMATCH;
        }
        /* 未达阈值，记录但继续运行 */
        return EMASTER_CONTROL_SESSION_OK;
    }
    if (session->dc_required &&
        !emaster_cycle_clock_observe_dc(&session->clock, session->context.DCtime)) {
        emaster_soem_session_latch_failure(session,
                                            EMASTER_CONTROL_SESSION_DC_SYNC_FAILED);
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_DC_SYNC_FAILED, true);
    }
    if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &next_deadline_ns)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        if (session->clock.deadline_missed)
        {
            note_deadline_missed(session);
            /*
             * 帧已收发完毕，仅周期末尾检查超限。本周期控制输出已写入，
             * 允许按策略恢复而不丢弃本次成果。
             */
            if (try_deadline_recovery(session))
            {
                return EMASTER_CONTROL_SESSION_OK;
            }
            emaster_soem_session_latch_failure(
                session, EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
        }
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             true);
    }
    return EMASTER_CONTROL_SESSION_OK;
}
