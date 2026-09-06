#include "session_internal.h"

#include <string.h>
#include <time.h>

static void note_deadline_missed(emaster_soem_session_t *session)
{
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        emaster_cyclic_timing_stats_note_deadline_missed(&session->axes[axis].timing);
    }
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
        (void)fail_exchange(session, phase, EMASTER_CONTROL_SESSION_WKC_MISMATCH, true);
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
    if (!matched) {
        return EMASTER_CONTROL_SESSION_WKC_MISMATCH;
    }
    if (session->dc_required &&
        !emaster_cycle_clock_observe_dc(&session->clock, session->context.DCtime)) {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_DC_SYNC_FAILED, true);
    }
    if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &next_deadline_ns)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        if (session->clock.deadline_missed)
        {
            note_deadline_missed(session);
        }
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             true);
    }
    return EMASTER_CONTROL_SESSION_OK;
}
