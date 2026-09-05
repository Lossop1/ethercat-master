#define _POSIX_C_SOURCE 200809L

#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 所有轴数组在网络周期之前建立，资源即使部分分配失败也由同一出口释放。 */
static emaster_control_session_status_t allocate_session(emaster_soem_session_t *session) {
    emaster_control_session_status_t status;

    session->images = calloc(session->plan->axis_count, sizeof(*session->images));
    session->controllers = calloc(session->plan->axis_count, sizeof(*session->controllers));
    session->controller_outputs =
        calloc(session->plan->axis_count, sizeof(*session->controller_outputs));
    session->status_words = calloc(session->plan->axis_count, sizeof(*session->status_words));
    if (session->images == NULL || session->controllers == NULL ||
        session->controller_outputs == NULL || session->status_words == NULL) {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        return status;
    }
    if (session->plan->motion_profile != NULL) {
        session->actual_positions =
            calloc(session->plan->axis_count, sizeof(*session->actual_positions));
        session->target_positions =
            calloc(session->plan->axis_count, sizeof(*session->target_positions));
        session->motion_scales = calloc(session->plan->axis_count, sizeof(*session->motion_scales));
        session->motion_axis_configs =
            calloc(session->plan->axis_count, sizeof(*session->motion_axis_configs));
        session->motion_axes = calloc(session->plan->axis_count, sizeof(*session->motion_axes));
        if (session->actual_positions == NULL || session->target_positions == NULL ||
            session->motion_scales == NULL || session->motion_axis_configs == NULL ||
            session->motion_axes == NULL) {
            status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
            return status;
        }
    }
    if (!emaster_multiaxis_coordinator_init(&session->coordinator, session->controllers,
                                            session->plan->axis_count)) {
        status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
        return status;
    }
    return EMASTER_CONTROL_SESSION_OK;
}

static void release_session(emaster_soem_session_t *session) {
    free(session->status_words);
    free(session->motion_axes);
    free(session->motion_axis_configs);
    free(session->motion_scales);
    free(session->target_positions);
    free(session->actual_positions);
    free(session->controller_outputs);
    free(session->controllers);
    free(session->io_map);
    emaster_cia_process_image_destroy(session->images, session->plan->axis_count);
}

/*
 * 入口只装配资源并依次调用生命周期阶段，不直接实现 PDO 字段处理、轨迹算法或
 * 故障诊断。业务参数均来自 plan；失败后始终先关闭会话，再释放内存。
 */
emaster_control_session_status_t emaster_soem_control_session(
    const emaster_session_plan_t *plan, emaster_control_session_axis_result_t *axis_storage,
    size_t axis_capacity, emaster_control_session_stop_requested_t stop_requested,
    void *stop_user_data, emaster_control_session_report_t *report) {
    emaster_soem_session_t storage;
    emaster_soem_session_t *session = &storage;
    emaster_control_session_status_t status;

    if (plan == NULL || plan->status != EMASTER_SESSION_PLAN_READY || plan->deployment == NULL ||
        plan->deployment->ethercat_interface == NULL || axis_storage == NULL ||
        axis_capacity < plan->axis_count || report == NULL || plan->axis_count == 0U ||
        plan->cycle_ns == 0U) {
        return EMASTER_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    memset(session, 0, sizeof(*session));
    memset(report, 0, sizeof(*report));
    memset(axis_storage, 0, plan->axis_count * sizeof(*axis_storage));
    session->plan = plan;
    session->axes = axis_storage;
    session->report = report;
    session->stop_requested = stop_requested;
    session->stop_user_data = stop_user_data;
    session->transition_cycles =
        ((uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000) + plan->cycle_ns - UINT64_C(1)) /
        plan->cycle_ns;
    session->op_transition_cycles = session->transition_cycles * UINT64_C(4);
    report->axes = axis_storage;
    report->axis_count = plan->axis_count;
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s",
                   plan->deployment->ethercat_interface);
    emaster_run_audit_init(&report->audit);

    status = allocate_session(session);
    if (status == EMASTER_CONTROL_SESSION_OK) {
        status = emaster_soem_session_configure(session);
    }
    if (status == EMASTER_CONTROL_SESSION_OK) {
        status = emaster_soem_session_start(session);
    }
    if (status == EMASTER_CONTROL_SESSION_OK && !report->stop_requested) {
        status = emaster_soem_session_run(session);
    }
    report->status = status;
    report->cycle_deadline_missed |= session->clock.deadline_missed;
    emaster_soem_session_shutdown(session);
    report->cycle_deadline_missed |= session->clock.deadline_missed;
    report->process_data_exchange_count = session->exchange;
    if (report->audit.allocation_failed && report->status == EMASTER_CONTROL_SESSION_OK) {
        report->status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
    }
    release_session(session);
    return report->status;
}

void emaster_control_session_report_destroy(emaster_control_session_report_t *report) {
    if (report != NULL) {
        emaster_run_audit_destroy(&report->audit);
    }
}
