#ifndef EMASTER_BUS_SOEM_SESSION_INTERNAL_H
#define EMASTER_BUS_SOEM_SESSION_INTERNAL_H

#include "cia_process_image.h"
#include "cycle_clock.h"
#include "emaster/bus/command_server.h"
#include "emaster/bus/control_session.h"
#include "emaster/config/error_recovery_config.h"
#include "emaster/multiaxis/coordinator.h"
#include "emaster/motion/velocity_profile.h"
#include "emaster/safety/gate.h"
#include "session_observer.h"

/*
 * 单次会话拥有全部总线资源和周期存储。入口负责分配和释放，配置、交换、控制、
 * 退出各阶段只借用这里的资源；这些内部类型不得传入 CiA 402 或轨迹模块。
 * 所有 SOEM 访问均由会话线程串行执行，周期期间不进行邮箱访问。
 */
typedef struct {
    const emaster_session_plan_t *plan;
    emaster_control_session_axis_result_t *axes;
    emaster_control_session_report_t *report;
    ecx_contextt context;
    emaster_cia_process_image_t *images;
    emaster_cia402_controller_t *controllers;
    emaster_cia402_output_t *controller_outputs;
    uint16_t *status_words;
    int32_t *actual_positions;
    int32_t *target_positions;
    emaster_position_scale_t *motion_scales;
    const emaster_motion_axis_config_t **motion_axis_configs;
    emaster_relative_motion_axis_t *motion_axes;
    emaster_relative_motion_t motion;
    emaster_velocity_axis_t *velocity_axes;
    emaster_velocity_motion_t velocity_motion;
    bool velocity_motion_prepared;
    emaster_multiaxis_coordinator_t coordinator;
    emaster_cycle_clock_t clock;
    uint8_t *io_map;
    uint64_t exchange;
    uint64_t transition_cycles;
    uint64_t op_transition_cycles;
    bool context_open;
    bool process_map_ready;
    bool sync0_started;
    bool cycle_output_active;
    bool dc_required;
    bool fault_latched;
    bool motion_prepared;
    emaster_control_session_stop_requested_t stop_requested;
    void *stop_user_data;
    emaster_control_session_state_changed_t state_changed;
    void *state_user_data;
    emaster_control_session_feedback_updated_t feedback_updated;
    void *feedback_user_data;
    emaster_control_session_position_target_source_t position_target_source;
    void *position_target_source_user_data;
    int32_t *position_target_source_targets;
    /* 0 表示调用者未启用跟随误差检查；非零时每周期对比实际位置与上一目标。 */
    uint64_t position_target_max_following_error_counts;
    /* 错误恢复策略配置：从部署配置加载，与业务逻辑解耦 */
    const emaster_error_recovery_policy_t *error_recovery_policy;
    /* WKC 错误恢复计数器 */
    uint64_t wkc_consecutive_errors;
    uint64_t wkc_total_errors;
    /* 实时命令服务器：运行期间接收外部命令（可选） */
    emaster_command_server_t *command_server;
} emaster_soem_session_t;

/* 原子更新会话状态并通知应用层；通知回调不得阻塞周期线程 */
void emaster_soem_session_set_state(
    emaster_soem_session_t *session,
    emaster_control_state_t state,
    emaster_control_session_status_t status);

/* 锁存会话的首个功能失败类别，并转换为统一安全门原因 */
void emaster_soem_session_latch_failure(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status);

/* 按运行模式读写轴目标和反馈，防止把速度或转矩冒充位置字段。 */
bool emaster_soem_axis_set_feedback(const emaster_session_axis_plan_t *plan,
                                    emaster_control_session_axis_result_t *axis,
                                    int32_t value);
int32_t emaster_soem_axis_target_value(const emaster_session_axis_plan_t *plan,
                                       const emaster_control_session_axis_result_t *axis);
bool emaster_soem_axis_set_target_value(const emaster_session_axis_plan_t *plan,
                                        emaster_control_session_axis_result_t *axis,
                                        int32_t value);

/* 运行监督器集中处理首错、反馈发布和整组轴安全门 */
void emaster_soem_session_note_runtime_failure(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status,
    emaster_control_session_status_t *first_status);
emaster_control_session_status_t emaster_soem_session_publish_feedback(
    emaster_soem_session_t *session,
    emaster_control_session_status_t status);
emaster_control_session_status_t emaster_soem_session_position_target_step(
    emaster_soem_session_t *session,
    bool *updated);
emaster_control_session_status_t emaster_soem_session_apply_safety(
    emaster_soem_session_t *session,
    bool feedback_valid,
    bool all_modes_confirmed,
    emaster_control_session_status_t current_status,
    bool *denied);

/* 非周期阶段：发现、PDO 布局、SDO 初始化和 DC 配置，最终到达 SAFE-OP。 */
emaster_control_session_status_t emaster_soem_session_configure(emaster_soem_session_t *session);
/* 建立周期、锁定初始位置并进入 OP；不会启动变化轨迹。 */
emaster_control_session_status_t emaster_soem_session_start(emaster_soem_session_t *session);
/* 一次周期交换只负责定时、传输和观测，不规划控制字或运动目标。 */
emaster_control_session_status_t emaster_soem_session_exchange(emaster_soem_session_t *session,
                                                               emaster_audit_phase_t phase);
/* 将周期反馈交给独立控制模块，生成下周期全轴输出。 */
emaster_control_session_status_t emaster_soem_session_run(emaster_soem_session_t *session);
/* 统一执行停用、诊断和总线关闭；不会覆盖首次运行失败。 */
void emaster_soem_session_shutdown(emaster_soem_session_t *session);
/* 运行时切换运动配置：重新初始化运动轨迹，从当前位置开始。 */
emaster_control_session_status_t emaster_soem_session_switch_motion(
    emaster_soem_session_t *session,
    const emaster_motion_profile_t *new_profile);

#endif
