#ifndef EMASTER_BUS_CONTROL_SESSION_H
#define EMASTER_BUS_CONTROL_SESSION_H

#include "emaster/cia402/controller.h"
#include "emaster/motion/relative_position.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CiA 402 控制会话的建立、周期运行和清理结果。 */
typedef enum
{
    EMASTER_CONTROL_SESSION_OK = 0,
    EMASTER_CONTROL_SESSION_INVALID_ARGUMENT,
    EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED,
    EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY,
    EMASTER_CONTROL_SESSION_NO_SLAVES,
    EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH,
    EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED,
    EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH,
    EMASTER_CONTROL_SESSION_PDO_MISMATCH,
    EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED,
    EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED,
    EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED,
    EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED,
    EMASTER_CONTROL_SESSION_OUT_OF_MEMORY,
    EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED,
    EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED,
    EMASTER_CONTROL_SESSION_INITIAL_WKC_FAILED,
    EMASTER_CONTROL_SESSION_OP_NOT_REACHED,
    EMASTER_CONTROL_SESSION_FEEDBACK_INVALID,
    EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
    EMASTER_CONTROL_SESSION_DRIVE_FAULT,
    EMASTER_CONTROL_SESSION_MOTION_INVALID,
    EMASTER_CONTROL_SESSION_FOLLOWING_ERROR,
    EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
    EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED,
    EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED
} emaster_control_session_status_t;

/*
 * 驱动故障对象来自供应商对象字典。read_succeeded 为 false 时，其余数值不得用于
 * 判断驱动状态；保留读取结果而不在总线层解释厂商故障码，避免协议执行与设备语义耦合。
 */
typedef struct
{
    bool read_succeeded;
    uint16_t cia402_error_code;
    uint8_t error_register;
    uint32_t extended_servo_error_code;
    uint32_t servo_error_code;
} emaster_drive_diagnostic_t;

/*
 * 对应 EtherCAT 同步管理器参数对象 1C32/1C33。计数器在同一次控制会话末尾读取，
 * 用于区分链路正常但主站周期不满足设备要求的情况。
 */
typedef struct
{
    bool read_succeeded;
    uint16_t sm_event_missed;
    uint16_t cycle_time_too_small;
    uint16_t shift_time_too_short;
    bool sync_error;
} emaster_sync_diagnostic_t;

typedef struct
{
    uint16_t position;
    bool identity_match;
    bool pdo_match;
    bool process_map_match;
    uint16_t pdo_assignment_failed_index;
    uint8_t pdo_assignment_failed_subindex;
    bool pdo_assignment_abort_code_available;
    uint32_t pdo_assignment_abort_code;
    bool output_initialized;
    bool input_decoded;
    bool mode_display_match;
    int8_t requested_mode;
    int8_t mode_display;
    bool mode_command_sdo_read;
    int8_t mode_command_sdo;
    bool mode_display_sdo_read;
    int8_t mode_display_sdo;
    bool input_mode_sdo_read;
    uint16_t input_mode_sdo;
    uint16_t status_word;
    uint16_t control_word;
    int32_t initial_actual_position;
    int32_t actual_position;
    int32_t target_position;
    int32_t motion_final_position;
    int32_t motion_completion_actual_position;
    int64_t motion_actual_delta_counts;
    uint64_t motion_final_error_counts;
    uint64_t max_following_error_counts;
    uint64_t max_observed_following_error_counts;
    bool position_scale_match;
    bool motion_direction_match;
    emaster_cia402_state_t cia402_state;
    bool switch_on_disabled_seen;
    bool ready_to_switch_on_seen;
    bool switched_on_seen;
    bool operation_enabled_seen;
    emaster_drive_diagnostic_t drive_diagnostic;
    emaster_sync_diagnostic_t sm2_diagnostic;
    emaster_sync_diagnostic_t sm3_diagnostic;
    emaster_position_scale_t position_scale;
} emaster_control_session_axis_result_t;

typedef struct
{
    emaster_control_session_status_t status;
    char interface_name[128];
    emaster_control_session_axis_result_t *axes;
    size_t axis_count;
    size_t io_map_size;
    uint16_t expected_wkc;
    int actual_wkc;
    uint64_t cycle_count;
    bool safe_op_reached;
    bool op_reached;
    bool all_axes_enabled_reached;
    bool motion_started;
    bool motion_completed;
    bool stop_requested;
    bool safe_output_sent;
    bool safe_state_reached;
    bool sync0_disabled;
    bool restore_init_succeeded;
} emaster_control_session_report_t;

/*
 * 停止回调由应用层实现，必须无阻塞且可从周期线程直接调用。返回 true 后，会话发送安全
 * 输出并退出周期循环。传入 NULL 表示持续运行，直到总线或驱动状态发生错误。
 */
typedef bool (*emaster_control_session_stop_requested_t)(void *user_data);

/*
 * 按部署计划建立 EtherCAT 过程数据会话。SAFE-OP 首帧会先从 6064 初始化 607A，防止 CSP
 * 使能时追逐零位置；进入 OP 后，每周期根据 6041 计算并发送 6040。部署未引用运动方案时持续
 * 保持启动位置；引用已批准方案时由独立轨迹模块生成全轴目标，完成后自动执行安全停止。
 * 任意失败和正常停止路径都会发送安全输出、关闭 Sync0 并请求所有从站恢复 INIT。
 */
emaster_control_session_status_t emaster_soem_control_session(
    const emaster_session_plan_t *plan,
    emaster_control_session_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_control_session_stop_requested_t stop_requested,
    void *stop_user_data,
    emaster_control_session_report_t *report);

#endif
