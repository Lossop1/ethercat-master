#define _POSIX_C_SOURCE 200809L

#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool emaster_soem_axis_set_feedback(const emaster_session_axis_plan_t *plan,
                                    emaster_control_session_axis_result_t *axis,
                                    int32_t value)
{
    if (plan == NULL || plan->operation_mode == NULL || axis == NULL) return false;
    if (plan->operation_mode->value == INT8_C(8)) axis->actual_position = value;
    else if (plan->operation_mode->value == INT8_C(9)) axis->actual_velocity = value;
    else if (plan->operation_mode->value == INT8_C(10) && value >= INT16_MIN && value <= INT16_MAX)
        axis->actual_torque = (int16_t)value;
    else return false;
    return true;
}

int32_t emaster_soem_axis_target_value(const emaster_session_axis_plan_t *plan,
                                       const emaster_control_session_axis_result_t *axis)
{
    if (plan == NULL || plan->operation_mode == NULL || axis == NULL) return 0;
    if (plan->operation_mode->value == INT8_C(8)) return axis->target_position;
    if (plan->operation_mode->value == INT8_C(9)) return axis->target_velocity;
    if (plan->operation_mode->value == INT8_C(10)) return axis->target_torque;
    return 0;
}

bool emaster_soem_axis_set_target_value(const emaster_session_axis_plan_t *plan,
                                        emaster_control_session_axis_result_t *axis,
                                        int32_t value)
{
    if (plan == NULL || plan->operation_mode == NULL || axis == NULL) return false;
    if (plan->operation_mode->value == INT8_C(8)) axis->target_position = value;
    else if (plan->operation_mode->value == INT8_C(9)) axis->target_velocity = value;
    else if (plan->operation_mode->value == INT8_C(10) && value >= INT16_MIN && value <= INT16_MAX)
        axis->target_torque = (int16_t)value;
    else return false;
    return true;
}

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
    if (session->plan->motion_profile != NULL || session->position_target_source != NULL) {
        session->actual_positions =
            calloc(session->plan->axis_count, sizeof(*session->actual_positions));
        session->target_positions =
            calloc(session->plan->axis_count, sizeof(*session->target_positions));
        session->motion_scales = calloc(session->plan->axis_count, sizeof(*session->motion_scales));
        session->motion_axis_configs =
            calloc(session->plan->axis_count, sizeof(*session->motion_axis_configs));
        session->motion_axes = calloc(session->plan->axis_count, sizeof(*session->motion_axes));
        session->velocity_axes = calloc(session->plan->axis_count, sizeof(*session->velocity_axes));
        if (session->actual_positions == NULL || session->target_positions == NULL ||
            session->motion_scales == NULL || session->motion_axis_configs == NULL ||
            session->motion_axes == NULL || session->velocity_axes == NULL) {
            status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
            return status;
        }
        if (session->position_target_source != NULL)
        {
            session->position_target_source_targets = calloc(
                session->plan->axis_count, sizeof(*session->position_target_source_targets));
            if (session->position_target_source_targets == NULL)
            {
                status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
                return status;
            }
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
    free(session->velocity_axes);
    free(session->motion_axis_configs);
    free(session->motion_scales);
    free(session->target_positions);
    free(session->position_target_source_targets);
    free(session->actual_positions);
    free(session->controller_outputs);
    free(session->controllers);
    free(session->io_map);
    emaster_cia_process_image_destroy(session->images, session->plan->axis_count);
}

void emaster_soem_session_set_state(
    emaster_soem_session_t *session,
    emaster_control_state_t state,
    emaster_control_session_status_t status)
{
    if (session == NULL || session->report == NULL || session->report->state == state)
    {
        return;
    }
    session->report->state = state;
    if (session->state_changed != NULL)
    {
        session->state_changed(state, status, session->state_user_data);
    }
}

void emaster_soem_session_latch_failure(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status)
{
    uint32_t reasons = EMASTER_SAFETY_REASON_FAULT_LATCHED;

    if (session == NULL || session->report == NULL ||
        status == EMASTER_CONTROL_SESSION_OK)
    {
        return;
    }
    switch (status)
    {
        case EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH:
        case EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH:
            reasons |= EMASTER_SAFETY_REASON_TOPOLOGY_UNVERIFIED;
            break;
        case EMASTER_CONTROL_SESSION_PDO_MISMATCH:
        case EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED:
            reasons |= EMASTER_SAFETY_REASON_PDO_UNVERIFIED;
            break;
        case EMASTER_CONTROL_SESSION_WKC_MISMATCH:
        case EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED:
        case EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED:
        case EMASTER_CONTROL_SESSION_OP_NOT_REACHED:
            reasons |= EMASTER_SAFETY_REASON_COMMUNICATION_INVALID;
            break;
        case EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED:
        case EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED:
        case EMASTER_CONTROL_SESSION_DC_SYNC_FAILED:
            reasons |= EMASTER_SAFETY_REASON_SYNCHRONIZATION_INVALID;
            break;
        case EMASTER_CONTROL_SESSION_FEEDBACK_INVALID:
        case EMASTER_CONTROL_SESSION_CONTROLLER_FAILED:
        case EMASTER_CONTROL_SESSION_DRIVE_FAULT:
            reasons |= EMASTER_SAFETY_REASON_FEEDBACK_INVALID;
            break;
        case EMASTER_CONTROL_SESSION_MOTION_INVALID:
        case EMASTER_CONTROL_SESSION_FOLLOWING_ERROR:
            reasons |= EMASTER_SAFETY_REASON_TARGET_INVALID;
            break;
        case EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE:
            reasons |= EMASTER_SAFETY_REASON_DRIVE_LIMIT_ACTIVE;
            break;
        default:
            break;
    }
    session->fault_latched = true;
    session->report->fault_latched = true;
    session->report->safety_control_permitted = false;
    session->report->safety_blocking_reasons |= reasons;
}

/*
 * 入口只装配资源并依次调用生命周期阶段，不直接实现 PDO 字段处理、轨迹算法或
 * 故障诊断。业务参数均来自 plan；失败后始终先关闭会话，再释放内存。
 */
emaster_control_session_status_t emaster_soem_control_session(
    const emaster_session_plan_t *plan, emaster_control_session_axis_result_t *axis_storage,
    size_t axis_capacity, const emaster_control_session_callbacks_t *callbacks,
    emaster_control_session_report_t *report) {
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
    if (callbacks != NULL) {
        session->stop_requested = callbacks->stop_requested;
        session->stop_user_data = callbacks->stop_user_data;
        session->state_changed = callbacks->state_changed;
        session->state_user_data = callbacks->state_user_data;
        session->feedback_updated = callbacks->feedback_updated;
        session->feedback_user_data = callbacks->feedback_user_data;
        session->position_target_source = callbacks->position_target_source;
        session->position_target_source_user_data = callbacks->position_target_source_user_data;
        session->position_target_max_following_error_counts =
            callbacks->position_target_max_following_error_counts;
        session->position_target_max_step_counts =
            callbacks->position_target_max_step_counts;
        session->external_target_buffer = callbacks->external_target_buffer;
    }
    session->transition_cycles =
        ((uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000) + plan->cycle_ns - UINT64_C(1)) /
        plan->cycle_ns;
    session->op_transition_cycles = session->transition_cycles * UINT64_C(4);

    /* 加载错误恢复策略：从部署配置引用，解耦容错参数与业务逻辑 */
    {
        const char *policy_id = plan->deployment->error_recovery_policy_id;
        if (policy_id == NULL) {
            policy_id = "default";
        }
        session->error_recovery_policy = emaster_error_recovery_policy_by_id(policy_id);
        /* 策略未找到时使用 NULL，exchange 逻辑将采用保守默认行为（首次错误即停机） */
    }

    /* 创建实时命令服务器：允许运行期间接收外部命令 */
    {
        char socket_path[256];
        (void)snprintf(socket_path, sizeof(socket_path), "/tmp/emaster-%s.sock",
                       plan->deployment->deployment_id);
        session->command_server = emaster_command_server_create(socket_path);
        if (session->command_server != NULL) {
            fprintf(stdout, "命令服务器已启动：%s\n", socket_path);
            (void)fflush(stdout);
        }
        /* 命令服务器创建失败不影响主站运行，只是无法接收实时命令 */
    }

    report->axes = axis_storage;
    report->axis_count = plan->axis_count;
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s",
                   plan->deployment->ethercat_interface);
    emaster_run_audit_init(&report->audit);
    report->state = EMASTER_CONTROL_STATE_INITIALIZING;
    if (session->state_changed != NULL) {
        session->state_changed(report->state, EMASTER_CONTROL_SESSION_OK,
                               session->state_user_data);
    }

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
    if (status != EMASTER_CONTROL_SESSION_OK) {
        emaster_soem_session_latch_failure(session, status);
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       status);
    }
    report->cycle_deadline_missed |= session->clock.deadline_missed;
    emaster_soem_session_shutdown(session);
    if (report->safe_state_reached && report->status == EMASTER_CONTROL_SESSION_OK) {
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_STOPPED,
                                       EMASTER_CONTROL_SESSION_OK);
    }
    report->cycle_deadline_missed |= session->clock.deadline_missed;
    report->process_data_exchange_count = session->exchange;
    if (report->audit.allocation_failed && report->status == EMASTER_CONTROL_SESSION_OK) {
        report->status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
        emaster_soem_session_latch_failure(session, report->status);
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       report->status);
    }
    release_session(session);
    return report->status;
}

void emaster_control_session_report_destroy(emaster_control_session_report_t *report) {
    if (report != NULL) {
        emaster_run_audit_destroy(&report->audit);
    }
}
