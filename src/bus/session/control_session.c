#define _POSIX_C_SOURCE 200809L

#include "session_internal.h"

#include "emaster/bus/command_socket_path.h"

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
    pthread_mutexattr_t attr;

    /* P4.5: 初始化错误环互斥锁，配置优先级继承以降低优先级反转风险 */
    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0 ||
        pthread_mutex_init(&session->error_ring_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
    }
    pthread_mutexattr_destroy(&attr);

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
    /*
     * 观测通道。开关关闭或分配失败时返回 false，会话照常继续——观测是旁路，它的
     * 缺席不该让控制回路停摆。返回值只用于报告自证（report.observation.enabled）。
     */
    (void)emaster_soem_session_observation_open(session);
    return EMASTER_CONTROL_SESSION_OK;
}

static void release_session(emaster_soem_session_t *session) {
    /* P4.5: 销毁错误环互斥锁 */
    emaster_soem_session_observation_close(session);
    (void)pthread_mutex_destroy(&session->error_ring_mutex);
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

/*
 * 首次锁存时保留故障瞬间的现场。这里是全部运行时失败的汇聚点，在此抓快照可以覆盖
 * 协调器拒绝整帧、轴健康检查和使能确认三类来源，不必在十几处调用点各写一遍。
 *
 * 只写一次：后续失败（含停机阶段）不得改变现场。控制器数组在 allocate_session
 * 成功前为 NULL，那时只记录计数、交换号和协调器判定。
 */
static void capture_runtime_failure(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status)
{
    emaster_runtime_failure_t *failure = &session->report->first_runtime_failure;
    size_t axis_index;
    size_t axis_count;

    if (failure->present)
    {
        return;
    }
    failure->present = true;
    failure->status = status;
    failure->cycle_count = session->report->cycle_count;
    failure->exchange = session->exchange;
    failure->coordinator_status = session->last_coordinator_status;
    if (session->plan == NULL || session->status_words == NULL ||
        session->controller_outputs == NULL)
    {
        return;
    }
    axis_count = session->plan->axis_count;
    if (axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES)
    {
        axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
    }
    failure->axis_count = axis_count;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        failure->status_words[axis_index] = session->status_words[axis_index];
        failure->control_words[axis_index] =
            session->controller_outputs[axis_index].control_word;
        failure->observed_states[axis_index] =
            session->controller_outputs[axis_index].observed_state;
        failure->state_known[axis_index] =
            session->controller_outputs[axis_index].state_known;
        failure->fault_present[axis_index] =
            session->controller_outputs[axis_index].fault_present;
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
    capture_runtime_failure(session, status);
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
    /* -1 而不是 0：0 是 MULTIAXIS_OK，"还没调用过"不能伪装成"协调器通过了" */
    session->last_coordinator_status = -1;
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
    /* 力矩上限是部署的原始字段（不经回调折算），与错误恢复策略同样从计划里取。 */
    session->torque_target_limit_per_mille =
        plan->deployment->external_target_torque_limit_per_mille;
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
    /*
     * 累计阈值的滑动窗口按策略配置的窗口长度建，两类故障各一个。放在这里而不是
     * 第一次出错时惰性初始化：那次初始化要读策略、要判长度变化，而它恰好发生在
     * 已经出错、最不该多做事的时刻。
     */
    {
        const emaster_error_recovery_policy_t *policy = session->error_recovery_policy;
        uint64_t wkc_window_ms = policy != NULL ? policy->wkc_recovery.total_error_window_ms : 0U;
        uint64_t no_frame_window_ms =
            policy != NULL ? policy->no_frame_recovery.total_error_window_ms : 0U;

        emaster_cyclic_window_init(&session->wkc_window, wkc_window_ms * UINT64_C(1000000));
        emaster_cyclic_window_init(&session->no_frame_window,
                                   no_frame_window_ms * UINT64_C(1000000));
        /*
         * P8.3: 把连续迟到的动作档位写进报告。0 表示只记账——报告必须自证走了
         * 哪条臂，否则"这轮为什么没停"只能靠翻配置文件回答。
         */
        session->report->dc_late_consecutive_threshold =
            (policy != NULL && policy->dc_late_recovery.enabled)
                ? policy->dc_late_recovery.consecutive_error_threshold
                : 0U;
    }

    /* 创建实时命令服务器：允许运行期间接收外部命令 */
    {
        /* 路径只有一个构造点（command_socket_path.h），CLI 工具用的是同一个函数。
         * 会话是唯一在这里建服务器的；main 那边不再另建一个——两个服务器绑同一路径
         * 时，后建的那个会 unlink 掉前者的套接字文件，先建的从此收不到任何连接。 */
        char socket_path[EMASTER_SOCKET_PATH_CAPACITY];

        if (!emaster_command_socket_path(plan->deployment->deployment_id,
                                         socket_path, sizeof(socket_path)))
        {
            socket_path[0] = '\0';
        }
        session->command_server =
            socket_path[0] != '\0' ? emaster_command_server_create(socket_path) : NULL;
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
    /*
     * 这里只登记上限，真正的封存在 prepare_audit 里做——它按运动时长算出容量，
     * 我们把这个容量压低到上限之内。见 run_audit.c 里那段"为什么需要一条上限"。
     */
    if (emaster_run_audit_apply_capacity_limit(&report->audit)) {
        fprintf(stdout, "审计记录上限：%zu 条（EMASTER_AUDIT_MAX_ACCESSES，实际容量在会话开始时定）\n",
                report->audit.capacity_limit);
        (void)fflush(stdout);
    }
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
    /*
     * 周期结束、停机诊断之前解封。顺序是硬约束：停机诊断走 record_access，那条路
     * 没有封存守卫，容量已满时会置 allocation_failed，而它被当作 AUDIT_FAILED 上报
     * 并让整轮会话转 FAULTED——刚刚跑完的一小时会被记成一次故障。
     */
    emaster_run_audit_end_cyclic(&report->audit);
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
