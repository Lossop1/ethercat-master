#include "session_internal.h"

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
    uint64_t now_ns;
    uint64_t deadline_ns;

    if (!emaster_cycle_clock_wait(&session->clock)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             false);
    }
    /* 唤醒后立即发送。审计不再位于定时点与发包之间，输出值在下一次编码前不变。 */
    ++session->exchange;
    (void)ecx_send_processdata(&session->context);
    session->report->actual_wkc = ecx_receive_processdata(&session->context, EC_TIMEOUTRET);
    ++session->report->cycle_count;
    matched = session->report->actual_wkc == (int)session->report->expected_wkc;
    if (!matched) {
        (void)fail_exchange(session, phase, EMASTER_CONTROL_SESSION_WKC_MISMATCH, true);
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
    emaster_session_mailbox_service(&session->mailbox);
    if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &deadline_ns)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             true);
    }
    return EMASTER_CONTROL_SESSION_OK;
}
