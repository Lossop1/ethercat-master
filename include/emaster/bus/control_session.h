#ifndef EMASTER_BUS_CONTROL_SESSION_H
#define EMASTER_BUS_CONTROL_SESSION_H

#include "emaster/audit/run_audit.h"
#include "emaster/cia402/controller.h"
#include "emaster/cyclic/timing.h"
#include "emaster/motion/relative_position.h"
#include "emaster/motion/position_target.h"
#include "emaster/session/session_plan.h"

#include <pthread.h>
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
    EMASTER_CONTROL_SESSION_WKC_MISMATCH,
    EMASTER_CONTROL_SESSION_OP_NOT_REACHED,
    EMASTER_CONTROL_SESSION_FEEDBACK_INVALID,
    EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
    EMASTER_CONTROL_SESSION_DRIVE_FAULT,
    EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE,
    EMASTER_CONTROL_SESSION_MOTION_INVALID,
    EMASTER_CONTROL_SESSION_FOLLOWING_ERROR,
    EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
    EMASTER_CONTROL_SESSION_DC_SYNC_FAILED,
    EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED,
    EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED,
    EMASTER_CONTROL_SESSION_AUDIT_FAILED,
    EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
} emaster_control_session_status_t;

/* 对外暴露稳定的产品生命周期，不要求调用者解析 SOEM 或 CiA 402 内部状态 */
typedef enum
{
    EMASTER_CONTROL_STATE_INITIALIZING = 0,
    EMASTER_CONTROL_STATE_SAFE_OP,
    EMASTER_CONTROL_STATE_OPERATIONAL,
    EMASTER_CONTROL_STATE_ENABLING,
    EMASTER_CONTROL_STATE_RUNNING,
    EMASTER_CONTROL_STATE_STOPPING,
    EMASTER_CONTROL_STATE_STOPPED,
    EMASTER_CONTROL_STATE_FAULTED
} emaster_control_state_t;

/* 每轴 DC 结果同时保存方案请求值、SOEM 生效状态和 ESC 寄存器读回值。 */
typedef struct
{
    bool requested;
    bool slave_capable;
    bool sync0_requested;
    bool sync0_active;
    bool register_read_succeeded;
    uint32_t requested_cycle_ns;
    uint32_t observed_cycle_ns;
    int32_t requested_shift_ns;
    int32_t soem_shift_ns;
    uint16_t requested_assign_activate;
    uint16_t observed_assign_activate;
} emaster_dc_axis_result_t;

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
 * 对应 EtherCAT 同步管理器参数对象 1C32/1C33。计数器在周期退出后读取，可能包含
 * 退出期间的事件，不能单独据此推断首次故障原因。
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
    uint32_t actual_vendor_id;
    uint32_t actual_product_code;
    uint32_t actual_revision;
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
    bool software_position_limits_read;
    int32_t software_position_limit_min;
    int32_t software_position_limit_max;
    bool following_error_read;
    int32_t following_error_actual;
    bool polarity_read;
    uint8_t polarity;
    /* 6041 的模式相关反馈位只做统一记录，具体语义由所选模式解释 */
    bool status_warning;
    bool voltage_enabled;
    bool remote;
    bool target_reached;
    bool internal_limit_active;
    uint8_t mode_specific_status;
    uint8_t manufacturer_specific_status;
    /* SAFE-OP 模式初始化后的即时 SDO 读回，用于区分 OP 前后的模式变化。 */
    bool safeop_mode_display_sdo_read;
    int8_t safeop_mode_display_sdo;
    /* SAFE-OP 模式读回同时保留当时的驱动诊断，不与结束时诊断混淆。 */
    emaster_drive_diagnostic_t safeop_drive_diagnostic;
    size_t final_diagnostic_read_count;
    size_t final_diagnostic_success_count;
    uint16_t status_word;
    uint16_t control_word;
    int32_t safeop_actual_position;
    int32_t initial_actual_position;
    int32_t actual_position;
    int32_t target_position;
    int32_t actual_velocity;
    int32_t target_velocity;
    int16_t actual_torque;
    int16_t target_torque;
    int16_t actual_current;
    uint32_t dc_link_voltage;
    int16_t mosfet_temperature;  /* 0.1°C, from 200Bh:01h */
    int16_t motor_temperature;   /* 0.1°C, from 200Bh:02h */
    int32_t motion_final_position;
    int32_t motion_completion_actual_position;
    int64_t motion_actual_delta_counts;
    uint64_t motion_final_error_counts;
    uint64_t max_following_error_counts;
    uint64_t max_observed_following_error_counts;
    bool position_scale_match;
    bool motion_direction_match;
    uint32_t output_bytes;
    uint32_t input_bytes;
    uint32_t output_bits;
    uint32_t input_bits;
    size_t output_offset_bytes;
    size_t input_offset_bytes;
    emaster_cia402_state_t cia402_state;
    bool switch_on_disabled_seen;
    bool ready_to_switch_on_seen;
    bool switched_on_seen;
    bool operation_enabled_seen;
    emaster_drive_diagnostic_t drive_diagnostic;
    emaster_sync_diagnostic_t sm2_diagnostic;
    emaster_sync_diagnostic_t sm3_diagnostic;
    emaster_position_scale_t position_scale;
    /* P4.4: 错误计数器（0x0300-0x030F），周期内 FPRD 读取 */
    bool error_counters_read;
    uint8_t rx_error_counter[8];
    uint16_t forwarded_rx_error_counter[4];
    uint8_t ecat_processing_unit_error_counter;
    uint8_t pdi_error_counter;
    uint8_t pdi_error_code;
    uint8_t lost_link_counter[4];
    /* P4.3: SDO 慢速通道观测数据（非 RT 线程周期读取） */
    bool sdo_current_read;
    int16_t sdo_current_6078h;
    bool sdo_voltage_read;
    uint32_t sdo_voltage_6079h;       /* DC link voltage, mV */
    bool sdo_mosfet_temp_read;
    int16_t sdo_mosfet_temp_200b01h;  /* MOSFET temperature, 0.1°C */
    bool sdo_motor_temp_read;
    int16_t sdo_motor_temp_200b02h;   /* Motor temperature, 0.1°C */
    uint64_t sdo_read_count;
    uint64_t sdo_read_time_us;
    emaster_dc_axis_result_t dc;
    /* 每个物理从站单独保存周期时序统计，传播延迟不能在多轴间混合。 */
    emaster_cyclic_timing_stats_t timing;
    /* 退出周期后、切换诊断状态前的 AL 快照，不冒充故障发生瞬间的状态。 */
    uint16_t shutdown_al_state;
    uint16_t shutdown_al_status_code;
} emaster_control_session_axis_result_t;

/* 首次周期失败只写一次，后续停机交换不能改变其 WKC、阶段和交换号。 */
typedef struct
{
    bool present;
    emaster_control_session_status_t status;
    emaster_audit_phase_t phase;
    uint64_t exchange;
    bool wkc_available;
    int wkc;
    int64_t dc_time_ns;
} emaster_cycle_failure_t;

typedef struct
{
    emaster_control_session_status_t status;
    emaster_control_state_t state;
    char interface_name[128];
    emaster_control_session_axis_result_t *axes;
    size_t axis_count;
    size_t io_map_size;
    uint16_t expected_wkc;
    int actual_wkc;
    uint64_t cycle_count;
    uint64_t process_data_exchange_count;
    bool cycle_deadline_missed;
    /* WKC 错误统计：支持容错分析 */
    uint64_t wkc_error_count;
    uint64_t wkc_consecutive_errors;
    uint64_t wkc_max_consecutive_errors;
    /* 安全门的最后一次判定，供上层明确知道为何禁止输出 */
    uint32_t safety_blocking_reasons;
    bool safety_control_permitted;
    bool fault_latched;
    bool dc_required;
    bool dc_configured;
    bool dc_startup_stable;
    uint16_t dc_reference_slave;
    uint32_t process_data_phase_ns;
    uint32_t dc_startup_cycles_requested;
    uint32_t dc_startup_cycles_completed;
    uint64_t dc_startup_exchanges;
    int64_t dc_startup_phase_error_ns;
    int64_t last_dc_time_ns;
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
    bool diagnostic_preop_reached;
    emaster_cycle_failure_t first_cycle_failure;
    emaster_run_audit_t audit;
} emaster_control_session_report_t;

/*
 * 停止回调由应用层实现，必须无阻塞且可从周期线程直接调用。返回 true 后，会话发送安全
 * 输出并退出周期循环。传入 NULL 表示持续运行，直到总线或驱动状态发生错误。
 */
typedef bool (*emaster_control_session_stop_requested_t)(void *user_data);

/* 生命周期通知与命令来源无关，回调必须无阻塞且不得访问 SOEM */
typedef void (*emaster_control_session_state_changed_t)(
    emaster_control_state_t state,
    emaster_control_session_status_t status,
    void *user_data);

/* 周期反馈只在回调执行期间有效，调用者如需跨线程使用必须自行复制 */
typedef struct
{
    uint64_t cycle;
    emaster_control_state_t state;
    emaster_control_session_status_t status;
    bool fault_latched;
    bool control_permitted;
    uint32_t blocking_reasons;
    const emaster_control_session_axis_result_t *axes;
    size_t axis_count;
} emaster_control_feedback_frame_t;

typedef void (*emaster_control_session_feedback_updated_t)(
    const emaster_control_feedback_frame_t *feedback,
    void *user_data);

/*
 * 应用层位置目标来源回调。
 *
 * 执行环境：主站周期线程（实时上下文，1ms周期）
 *
 * 严格约束（违反将导致主站失效）：
 * - 执行时间：必须在 10 微秒内返回（推荐 < 5us）
 * - 无阻塞操作：禁止 I/O、互斥锁、条件变量、信号量
 * - 无动态分配：禁止 malloc/free/new/delete
 * - 无系统调用：禁止文件操作、网络操作、时间查询
 * - 不得访问 SOEM：axes 参数是只读快照，不得调用 EtherCAT API
 *
 * 参数：
 * - cycle: 周期计数器（从0开始）
 * - axes: 当前周期的轴状态快照（只读），包含实际位置、状态字等
 * - axis_count: 轴数量
 * - target_positions: 输出缓冲区（由回调填充），单位为编码器 counts
 * - target_capacity: 输出缓冲区容量（当前总是等于 axis_count）
 * - user_data: 用户自定义数据指针
 *
 * 返回值：
 * - EMASTER_POSITION_TARGET_SOURCE_UPDATED: 已填充新目标到 target_positions
 * - EMASTER_POSITION_TARGET_SOURCE_HOLD: 本周期无新目标，保持当前位置
 * - 其他值: 视为错误，触发安全停机
 *
 * 错误传播：
 * - 回调返回错误 → 主站设置 MOTION_INVALID 状态 → 安全门阻止输出 → 主站停止
 * - 后验证失败（限位、速率） → 同上
 * - 硬件写入失败 → 同上
 *
 * 线程安全：
 * - 回调在主站周期线程中执行（与会话状态同线程）
 * - user_data 必须对回调线程可见且线程安全
 * - 如需与其他线程通信，使用无锁数据结构或原子操作
 *
 * 示例用法：
 *
 *   // 静态缓冲区（避免动态分配）
 *   static int32_t external_targets[MAX_AXES];
 *   static _Atomic bool targets_ready = false;
 *
 *   emaster_position_target_source_result_t my_callback(
 *       uint64_t cycle,
 *       const emaster_control_session_axis_result_t *axes,
 *       size_t axis_count,
 *       int32_t *target_positions,
 *       size_t target_capacity,
 *       void *user_data)
 *   {
 *       if (!atomic_load(&targets_ready)) {
 *           return EMASTER_POSITION_TARGET_SOURCE_HOLD;
 *       }
 *
 *       for (size_t i = 0; i < axis_count; i++) {
 *           target_positions[i] = external_targets[i];
 *       }
 *
 *       return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
 *   }
 *
 * target_positions 只有在返回 UPDATED 时才会被采用，单位为所选 PDO 的原始位置计数；
 * 这个最小接口不规定上层输入频率、传输协议或命令生产者。
 */
typedef emaster_position_target_source_result_t (*emaster_control_session_position_target_source_t)(
    uint64_t cycle,
    const emaster_control_session_axis_result_t *axes,
    size_t axis_count,
    int32_t *target_positions,
    size_t target_capacity,
    void *user_data);

/*
 * 外部位置目标双缓冲区：由上层（工具或应用）分配和初始化，
 * 通过 callbacks 注入会话；总线层只持有指针，不拥有内存。
 * 使用方负责互斥锁的 PTHREAD_MUTEX_INITIALIZER 初始化。
 */
#define EMASTER_EXTERNAL_TARGET_MAX_AXES 16U

typedef struct {
    pthread_mutex_t     mutex;
    int32_t             positions[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    uint64_t            last_update_ns;
    size_t              axis_count;
    int                 available;
} emaster_external_target_buffer_t;

/* 应用层回调集中管理，不改变主会话函数签名。 */
typedef struct
{
    emaster_control_session_stop_requested_t stop_requested;
    void *stop_user_data;
    emaster_control_session_state_changed_t state_changed;
    void *state_user_data;
    emaster_control_session_feedback_updated_t feedback_updated;
    void *feedback_user_data;
    emaster_control_session_position_target_source_t position_target_source;
    void *position_target_source_user_data;
    /*
     * 位置目标来源回调路径的可选跟随误差保护。固定方案路径通过 motion_profile 的
     * max_following_error_millidegrees 限制；回调路径没有绑定运动方案，由调用者
     * 在这里单独提供每轴统一的上限（原始位置计数，0 表示不启用此检查）。
     * 主站每周期以上一条已写入的目标为基准计算偏差，超限时触发 FOLLOWING_ERROR。
     */
    uint64_t position_target_max_following_error_counts;
    /*
     * 位置目标来源回调路径的可选单步限幅。限制相邻两条目标之间的最大增量（原始位置计数），
     * 防止外部目标发生任意大跳变；0 表示不启用。超限时拒绝本条目标并触发 MOTION_INVALID。
     */
    uint64_t position_target_max_step_counts;
    /*
     * 外部目标双缓冲区：由调用方分配并用 PTHREAD_MUTEX_INITIALIZER 初始化，
     * 生命期须覆盖整个会话；NULL 表示本次会话不使用外部目标通道。
     */
    emaster_external_target_buffer_t *external_target_buffer;
} emaster_control_session_callbacks_t;

/*
 * 按部署计划建立 EtherCAT 过程数据会话。SAFE-OP 首帧会先从 6064 初始化 607A，防止 CSP
 * 使能时追逐零位置；进入 OP 后，每周期根据 6041 计算并发送 6040。部署未引用运动方案时持续
 * 保持启动位置；引用已批准方案时由独立轨迹模块生成全轴目标，完成后自动执行安全停止。
 * position_target_source 回调存在时，OP 中的 CSP 目标改由该回调按周期提供，固定轨迹不再推进。
 * 任意失败和正常停止路径都会尝试安全停用、关闭 Sync0 并请求恢复 INIT，结果分别记录。
 */
emaster_control_session_status_t emaster_soem_control_session(
    const emaster_session_plan_t *plan,
    emaster_control_session_axis_result_t *axis_storage,
    size_t axis_capacity,
    const emaster_control_session_callbacks_t *callbacks,
    emaster_control_session_report_t *report);

/* 调用者完成报告输出后必须释放会话内部自动采集的审计记录。 */
void emaster_control_session_report_destroy(emaster_control_session_report_t *report);

#endif
