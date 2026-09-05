#ifndef EMASTER_BUS_SOEM_SESSION_INTERNAL_H
#define EMASTER_BUS_SOEM_SESSION_INTERNAL_H

#include "cia_process_image.h"
#include "cycle_clock.h"
#include "emaster/bus/control_session.h"
#include "emaster/multiaxis/coordinator.h"
#include "session_mailbox.h"
#include "session_observer.h"

/*
 * 单次会话拥有全部总线资源和周期存储。入口负责分配和释放，配置、交换、控制、
 * 退出各阶段只借用这里的资源；这些内部类型不得传入 CiA 402 或轨迹模块。
 * PDO 由调用线程串行交换，固定 PDO 模式反馈通过独立的 SOEM 邮箱工作线程读取。
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
    emaster_multiaxis_coordinator_t coordinator;
    emaster_cycle_clock_t clock;
    emaster_session_mailbox_t mailbox;
    uint8_t *io_map;
    uint64_t exchange;
    uint64_t transition_cycles;
    uint64_t op_transition_cycles;
    bool context_open;
    bool process_map_ready;
    bool sync0_started;
    bool cycle_output_active;
    bool dc_required;
    emaster_control_session_stop_requested_t stop_requested;
    void *stop_user_data;
} emaster_soem_session_t;

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

#endif
