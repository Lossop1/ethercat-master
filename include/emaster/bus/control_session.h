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
    EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED,
    EMASTER_CONTROL_SESSION_ALL_AXES_FAULTED
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

/* P2.5: 单轴状态，用于故障隔离和恢复 */
typedef enum
{
    EMASTER_AXIS_STATUS_NORMAL = 0,     /* 正常运行 */
    EMASTER_AXIS_STATUS_FAULTED,        /* 故障，已隔离 */
    EMASTER_AXIS_STATUS_RECOVERING,     /* 恢复中 */
    EMASTER_AXIS_STATUS_ISOLATED        /* 手动隔离（预留） */
} emaster_axis_status_t;

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
    /* 增益参数 (2008h) - 从 SAFE-OP 阶段读取 */
    bool gain_parameters_read;
    uint16_t velocity_loop_kp;   /* 2008h:01h, 0.01 unit */
    uint16_t velocity_loop_ki;   /* 2008h:02h, 0.01 unit */
    uint16_t velocity_loop_kd;   /* 2008h:03h, 0.01 unit */
    uint16_t position_loop_kp;   /* 2008h:04h, 0.01 unit */
    uint16_t position_loop_ki;   /* 2008h:05h, 0.01 unit */
    uint16_t position_loop_kd;   /* 2008h:06h, 0.01 unit */
    uint16_t current_loop_kp;    /* 2008h:07h, 0.01 unit */
    uint16_t current_loop_ki;    /* 2008h:08h, 0.01 unit */
    uint16_t current_loop_kd;    /* 2008h:09h, 0.01 unit */
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
    /*
     * 运行期周期性探测的 1C32/1C33 结果。上面两个 sm2/sm3_diagnostic 只在停机后读一次，
     * 可能包含退出期间的事件，回答不了"驱动器是在运行期就已经带上同步错误，还是丢失只出现在
     * 停机瞬间"。这里记运行期首次读到非零的交换号（0 = 全程未出现）与最后一次读到的值，
     * 用来和 first_cycle_failure 的时间坐标对齐。
     */
    uint64_t sm_sync_probe_count;
    uint64_t sm2_first_error_exchange;
    uint16_t sm2_first_error_missed;
    bool sm2_first_error_sync_error;
    uint16_t sm2_last_missed;
    bool sm2_last_sync_error;
    uint64_t sm3_first_error_exchange;
    uint16_t sm3_first_error_missed;
    bool sm3_first_error_sync_error;
    uint16_t sm3_last_missed;
    bool sm3_last_sync_error;
    /*
     * 第一个 WKC 不符当下的 AL 快照，回答"驱动器此刻还在不在 OP"。更进一步的问法是
     * 驱动器自己的 SM2 事件丢失计数此刻是多少（帧异常在前则接近 0，驱动器监督在前则
     * 已接近阈值），但那只能走邮箱读 1C32:11，而在周期里发邮箱本身就是推出 OP 的
     * 原因之一（见 session_exchange.c 现场函数的说明），故不在此处取。
     */
    bool first_mismatch_al_read;
    uint16_t first_mismatch_al_state;
    uint16_t first_mismatch_al_status_code;
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
    bool sdo_motor_speed_read;
    int32_t sdo_motor_speed_200b08h;  /* Actual motor speed, rpm */
    bool sdo_speed_command_read;
    int32_t sdo_speed_command_200b09h; /* Speed command, rpm */
    uint64_t sdo_read_count;
    uint64_t sdo_read_time_us;
    /* P2.5: 轴级故障恢复 */
    emaster_axis_status_t axis_status;
    bool fault_isolated;               /* 此轴已隔离，不影响其他轴 */
    uint64_t fault_cycle;              /* 故障发生的周期号 */
    emaster_control_session_status_t fault_reason; /* 故障原因 */
    emaster_dc_axis_result_t dc;
    /* 每个物理从站单独保存周期时序统计，传播延迟不能在多轴间混合。 */
    emaster_cyclic_timing_stats_t timing;
    /* 退出周期后、切换诊断状态前的 AL 快照，不冒充故障发生瞬间的状态。 */
    uint16_t shutdown_al_state;
    uint16_t shutdown_al_status_code;
    /*
     * 停机序言的两个附加 AL 坐标：刚进入停机（应力来自周期运行）与发出第一个安全
     * 停机帧之前（应力来自序言阻塞）。只靠第三个坐标无法区分"周期运行中掉出 OP"和
     * "序言里停机数据流中断导致掉出 OP"，而两者的修法完全相反。
     */
    uint16_t shutdown_entry_al_state;
    uint16_t shutdown_entry_al_status_code;
    uint16_t shutdown_pre_stop_al_state;
    uint16_t shutdown_pre_stop_al_status_code;
} emaster_control_session_axis_result_t;

/* 周期现场环的容量。64 条约等于 64 ms，够覆盖一次掉出 OP 前后的窗口。 */
#define EMASTER_CYCLE_TRACE_CAPACITY 64U

/* 周期现场样本的标志位。截止超时不在其中：它由报告里带交换号的标量单独记录。 */
#define EMASTER_CYCLE_TRACE_FLAG_WKC_MISMATCH 0x0001U
#define EMASTER_CYCLE_TRACE_FLAG_ERROR_COUNTER_READ_FAILED 0x0002U

/*
 * 一个周期的一条现场记录。报告此前只有整段运行的极值和直方图：1.077 ms 的往返、
 * 103 µs 的发送迟到、45 µs 的 SYNC0 裕量都没有交换号坐标，周期尾部也没有分成
 * 收包 / 邮箱推进 / 3×FPRD(0x0300) 三段（而 timing 的 round_trip 正好把后两段
 * 都算了进去）。于是"驱动器在那几个周期里到底有没有按时拿到数据"无法从报告回答。
 */
typedef struct
{
    uint64_t exchange;
    int32_t wkc;
    uint32_t flags;
    uint32_t send_duration_ns;
    uint32_t receive_duration_ns;
    uint32_t mailbox_duration_ns;
    uint32_t error_counter_duration_ns;
    int32_t send_lateness_ns;
    int32_t sync0_margin_ns;
    /*
     * 本周期与上一周期发帧之间的实际间隔。放进现场环是为了让掉出 OP 之前那 64 个周期
     * 能直接读出"驱动器看到的帧距"——只记主站内部分段的话，整周期跳发这种故障在
     * 现场里完全没有痕迹。
     */
    uint32_t frame_interval_ns;
} emaster_cycle_trace_sample_t;

/*
 * 环形覆盖的周期现场。会话结束时环里就是最后 64 个周期（故障窗口本身）；
 * 第一个 WKC 不符当下另冻结一份副本，那份记录的是不符之前的 64 个周期——
 * 环会被后续周期覆盖，不冻结就没有异常之前的历史。
 */
typedef struct
{
    emaster_cycle_trace_sample_t samples[EMASTER_CYCLE_TRACE_CAPACITY];
    size_t sample_count;
    size_t write_index;
    bool mismatch_present;
    uint64_t mismatch_exchange;
    int mismatch_wkc;
    size_t mismatch_sample_count;
    emaster_cycle_trace_sample_t mismatch_samples[EMASTER_CYCLE_TRACE_CAPACITY];
} emaster_cycle_trace_t;

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

/* 运行时故障现场记录的轴数上限，与过程映像和外部目标缓冲保持一致。 */
#define EMASTER_RUNTIME_FAILURE_MAX_AXES 16U

/*
 * 首次运行时故障现场。first_cycle_failure 只覆盖周期交换自身的失败；多轴协调器、
 * 轴健康检查和使能确认发现的故障走 latch_failure 那条路径，此前不留下任何现场，
 * 事后只能从一句笼统的失败消息反推。这里在首次锁存时抓一次快照。
 *
 * coordinator_status 是判据的核心：非零（非 MULTIAXIS_OK）表示故障由协调器拒绝整帧
 * 引起，此时协调器已把全部输出清零，所以所有轴的 state_known 都为假；为零则说明
 * 故障来自其后的轴健康检查（该轴 state_known 为假）或使能确认（voltage_enabled 为假）。
 * status_words 是喂给协调器的原始状态字，不被清零，据此可以定位是哪一轴解不出来。
 *
 * 这些数组只在前 axis_count 项有效，超出部分保持零值。
 */
typedef struct
{
    bool present;
    emaster_control_session_status_t status;
    uint64_t cycle_count;
    uint64_t exchange;
    /* 多轴协调器的最后一次判定，取值见 emaster_multiaxis_status_t。 */
    int coordinator_status;
    size_t axis_count;
    uint16_t status_words[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    uint16_t control_words[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    emaster_cia402_state_t observed_states[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    bool state_known[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    bool fault_present[EMASTER_RUNTIME_FAILURE_MAX_AXES];
} emaster_runtime_failure_t;

/*
 * 停机序言的分段计时。停机序言（最后一次周期交换 → 第一条安全停机帧）期间不发送任何
 * 过程数据，驱动器靠过程数据的连续性维持 OP：窗口超过同步容差就以 AL 0x1A 掉出 SAFE-OP，
 * 之后停机帧再正确也无效。2026-09-15 四轴 12 臂实测的缺口是 1.0 ms（通过）对 5.0～6.0 ms
 * （失败），而 join 只占 0.25～1.93 ms，主项在 join 之外的序言工作里——两次 AL 快照
 * （各自一次 ecx_readstate）加审计收尾。只量总缺口无法判断该砍哪一段，所以逐段打点。
 *
 * 所有 mark_* 都是相对序言起点的纳秒数（起点 = 最后一次周期帧的发送结束时刻）；
 * start_valid 为假时全部保持 0，表示本轮没有可用的起点，不是"耗时为零"。
 */
typedef struct
{
    bool start_valid;
    /* 本轮是否走 EMASTER_SHUTDOWN_FAST 快路径（不把两次 AL 快照放进断供窗口）。 */
    bool fast_mode;
    /*
     * 本轮是否走 EMASTER_SHUTDOWN_INLINE_PROLOGUE：序言融进周期，边发帧边准备。
     * 置位时下面这组 mark_* 一律不取（保持 0 = 没测到）：序言工作发生在帧与帧之间，
     * 不是断供窗口的一部分，按缺口原点记只会得到一堆贴着 0 的数。序言本身有多长、
     * 发了几帧，看 inline_drain_ns / inline_drain_cycles；断供窗口仍然只看
     * result.shutdown_prologue_gap_ns（它的含义在两种模式下相同）。
     */
    bool inline_mode;
    /* inline 模式下序言的总长与发包数：从最后一条周期帧到序言做完，中间一直有帧。 */
    uint64_t inline_drain_ns;
    uint64_t inline_drain_cycles;
    uint64_t mark_after_entry_al_ns;
    uint64_t mark_after_join_ns;
    uint64_t mark_after_pre_stop_al_ns;
    uint64_t mark_after_audit_ns;
    /* 第一条安全停机帧的发送结束时刻；有值时等于 result.shutdown_prologue_gap_ns。 */
    uint64_t mark_first_safe_frame_ns;
    /* 两次 ecx_readstate 快照自身的耗时，用来验证"快照是缺口主项"这个猜测。 */
    uint64_t entry_al_read_ns;
    uint64_t pre_stop_al_read_ns;
    /*
     * 序言起点的 CLOCK_MONOTONIC 绝对值。上面所有 mark_* 都以它为 0，而停机首拍仪表
     * （见 emaster_shutdown_attempt_t）记的是时钟线程自己的绝对时刻；两边要放在同一条
     * 时间轴上比，得有一个公共原点。
     */
    uint64_t origin_monotonic_ns;
} emaster_shutdown_prologue_t;

/*
 * 停机循环的逐周期现场。停机循环最多跑 max_cycles（EC_TIMEOUTSTATE / 周期）拍，而值得看
 * 的是开头几拍：12 臂实测里四轴在"安全停机前"仍为 OP，掉出只可能发生在循环内部，报告
 * 里却只有循环前后的两个坐标。环保留前 EMASTER_SHUTDOWN_CYCLE_CAPACITY 拍（覆盖掉出
 * 那一刻），另存最后一拍（循环跑到超时的情形）。全部只写内存，循环里不做 I/O。
 */
#define EMASTER_SHUTDOWN_CYCLE_CAPACITY 24U

/*
 * 停机首拍"这一拍为什么这么久"的定位仪表。
 *
 * 2026-09-15 正式批次实测：从"审计收尾后"到第一条安全停机帧之间稳定地跑掉 3.7～4.0 ms，
 * 而这段里唯一的阻塞点是 emaster_cycle_clock_wait 的 clock_nanosleep——周期时钟在这一拍
 * 里要么等了一次重新对齐后的边界，要么主线程在进入交换之前就停了。两种解释的修法完全
 * 不同，光看总缺口分不开，所以记四个时刻：
 *
 *   begin_ns      这一拍开始（控制器步进之前）的时刻
 *   deadline_*    进入这次交换之前 / 交换返回之后，周期时钟自己认的下一个发送边界
 *   end_ns        交换返回的时刻
 *
 * 三点连读就能定性：begin 到 end 之间的空档若全在交换里，说明是等待（看 deadline 前后
 * 跳了几个周期）；若 begin 本身就已经离序言起点很远，说明是交换之前的停顿。
 * 前 EMASTER_SHUTDOWN_ATTEMPT_CAPACITY 拍都记：第 0 拍可能是被整周期跳发的那一拍
 * （交换号不推进），第一条真正发出去的停机帧在下一拍。
 *
 * 所有时刻都是 CLOCK_MONOTONIC 绝对值；换算成相对序言起点的位置要用
 * shutdown_prologue.origin_monotonic_ns。
 */
#define EMASTER_SHUTDOWN_ATTEMPT_CAPACITY 2U

typedef struct
{
    uint64_t begin_ns;
    uint64_t deadline_before_ns;
    uint64_t deadline_after_ns;
    uint64_t end_ns;
    /* 交换返回后的交换号：与 begin 前的差就是这一拍到底有没有发出帧。 */
    uint64_t exchange_after;
    int32_t exchange_status;
    /* 周期时钟的 PI 状态，用来判断跳发幅度是不是被修正量放大过。 */
    int64_t correction_ns;
    int64_t phase_error_ns;
} emaster_shutdown_attempt_t;

typedef struct
{
    uint64_t exchange;
    int32_t wkc;
    /* 本拍停机交换的返回码（emaster_control_session_status_t）；非 0 即本拍中止停机循环。 */
    int exchange_status;
    bool all_axes_safe;
    /*
     * 本拍的逐轴状态字是否已经由本拍的回帧解码过。交换失败而提前返回的那一拍，
     * 状态字仍是上一拍的值，此时为假——否则会把上一拍的状态当成这一拍的现场。
     */
    bool axes_decoded;
    size_t axis_count;
    uint16_t status_words[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    uint16_t control_words[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    /* 解码失败（status word 解不出 CiA402 状态）的轴，标量状态列无效，看 states_known。 */
    uint8_t cia402_states[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    bool states_known[EMASTER_RUNTIME_FAILURE_MAX_AXES];
} emaster_shutdown_cycle_sample_t;

typedef struct
{
    /* 停机循环实际跑了多少拍（含未入环的中止拍）。 */
    uint64_t cycle_total;
    size_t sample_count;
    emaster_shutdown_cycle_sample_t samples[EMASTER_SHUTDOWN_CYCLE_CAPACITY];
    bool last_valid;
    emaster_shutdown_cycle_sample_t last;
    /*
     * 循环内首次 WKC 不符当下的 AL 快照：把"驱动器什么时候掉出 OP"钉到周期粒度。
     * 这次读取本身要占断供窗口的时间，耗时记账在 mismatch_al_read_ns 里，读数要扣掉它。
     */
    bool mismatch_al_read;
    uint64_t mismatch_al_read_ns;
    uint64_t mismatch_al_exchange;
    size_t mismatch_al_axis_count;
    uint16_t mismatch_al_state[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    uint16_t mismatch_al_status_code[EMASTER_RUNTIME_FAILURE_MAX_AXES];
    /* 交换前就中止的拍（控制器步进或过程映像更新失败）：没有帧，也没有现场可记。 */
    bool aborted_before_exchange;
    size_t aborted_axis;
    int aborted_stage;
    size_t attempt_count;
    emaster_shutdown_attempt_t attempts[EMASTER_SHUTDOWN_ATTEMPT_CAPACITY];
} emaster_shutdown_cycle_trace_t;

/*
 * 停止观测线程时的相位现场。join 的耗时是双峰的（12 臂实测 ≤0.806 ms 全通过、
 * ≥0.935 ms 全失败），而观测线程只在固定检查点看停止标志：睡眠片的边界、每次邮箱读的
 * 入口、以及读返回之后。所以 join =（标志落下 → 被看见）+（看见 → 线程退出）。
 *
 * 两个坐标都要：
 * - stop_phase 是"标志落下那一刻"线程在做什么：它由当前相位的起点反推（标志时刻落在
 *   当前相位区间内才算数，否则记 UNKNOWN，不猜）。多等一个睡眠片与"卡在解算/日志 I/O 里"
 *   是两种完全不同的修法。
 * - stop_site 是"在哪一类检查点上被看见"，与 stop_phase 不同：相位是事实，检查点是机制。
 */
#define EMASTER_OBSERVER_PHASE_UNKNOWN 0
#define EMASTER_OBSERVER_PHASE_READ 1    /* 邮箱往返中（含同步探针的读） */
#define EMASTER_OBSERVER_PHASE_SAMPLE 2  /* 解算并写样本，含每 20 次的 stderr 日志 */
#define EMASTER_OBSERVER_PHASE_SLEEP 3   /* 睡眠片中（含探针段的轮间） */

#define EMASTER_OBSERVER_STOP_SITE_SLEEP 0
#define EMASTER_OBSERVER_STOP_SITE_MAILBOX 1
#define EMASTER_OBSERVER_STOP_SITE_BETWEEN 2

typedef struct
{
    /* 主线程置停止标志的时刻（join 计时的起点与之相差一次 clock_gettime）。 */
    uint64_t stop_flag_ns;
    /* 标志落下那一刻观测线程的相位与相位起点（见上面的 PHASE_*）。 */
    int stop_phase;
    uint64_t stop_phase_begin_ns;
    /* 观测线程第一次看见标志的时刻、在哪类检查点上看见、当时在处理哪一轴。 */
    uint64_t stop_seen_ns;
    int stop_site;
    size_t stop_axis;
    uint64_t stop_iteration;
    /* 看见标志的时刻 → 循环退出 → 线程函数返回。差值就是线程收尾自身的开销。 */
    uint64_t loop_exit_ns;
    uint64_t exit_ns;
    /* 整轮的规模与极值：读次数、单次邮箱往返最大耗时、单轮最大耗时。 */
    uint64_t iteration_count;
    uint64_t read_count;
    uint64_t read_max_ns;
    uint64_t read_last_ns;
    uint64_t iteration_max_ns;
    /* 观测线程当前相位与正在处理的轴（只有它自己写，停机后主线程才读，无需加锁）。 */
    int phase;
    uint64_t phase_begin_ns;
    size_t current_axis;
} emaster_observer_stop_trace_t;

/*
 * 收尾时一次性抓的各线程调度累计值（/proc/self/task/<tid>/schedstat）。
 * wait_ns 是"在运行队列上等着被调度"的累计时间，正是同优先级 SCHED_FIFO 线程互相
 * 遮挡的度量；只在停机之后读，不在任何实时窗口内。
 */
#define EMASTER_THREAD_SCHEDSTAT_CAPACITY 8U

typedef struct
{
    uint32_t tid;
    int policy;
    /*
     * 内核的**原始**优先级，不是 schedule_priority。对 SCHED_FIFO/SCHED_RR，
     * 内核按 `99 - sched_priority` 存，所以配置里的 80 在这里读出来是 19。
     * 保持原值不换算：换算公式对 SCHED_OTHER 不成立，而这一列的价值恰恰是
     * 它跟内核说的话一字不差。要对照配置就读成 `99 - priority`。
     */
    int priority;
    /*
     * 亲和掩码（/proc/self/task/<tid>/status 的 Cpus_allowed_list，形如 "11" 或
     * "0-11"）。policy/prio 说得清调度策略，说不清"绑没绑上、绑在哪个核"，而部署
     * 的 realtime.required 为假时绑定失败是允许继续运行的——那种轮次里，这一列是
     * 报告内唯一能证明线程实际跑在哪些核上的东西，且它是内核的实况而非进程自述。
     */
    char cpus_allowed[64];
    uint64_t exec_ns;
    uint64_t wait_ns;
    uint64_t switches;
} emaster_thread_schedstat_t;

/*
 * 观测通道的自证段。
 *
 * 存在的理由与 shutdown_prologue 里的 fast_mode/inline_mode 完全一样：一轮 A/B 结果
 * 必须能在报告里自证走的是哪条臂，而不是靠命令行复述或日志。除此之外，这里还留下
 * "发布这一行到底贵不贵"的直接证据——发布发生在周期线程内，它的成本属于控制回路，
 * 不该只存在于离线自测的结论里。
 *
 * 为什么不记分位数：分位要在实时路径上维护直方图或缓存样本，而周期路径上多一个
 * 数据结构的代价比多一条比较大得多。取极值 + 超阈值计数回答的是同一个问题（"贵到
 * 什么程度、超了多少次"），代价是一次比较和一次可能加的加法。台架判据里的分位由
 * 这两条计数与 published_frames 反推，不需要实时期维护。
 */
typedef struct
{
    /* 本轮开关是否打开（EMASTER_OBSERVATION）。关闭时其余字段全为 0。 */
    bool enabled;
    /* 环形缓冲槽数，= 历史窗口的周期数。 */
    uint32_t ring_capacity;
    /* 实际发布出去的帧数。不等于 cycle_count：截止恢复跳发的拍不发帧。 */
    uint64_t published_frames;
    uint64_t first_publish_exchange;
    /* 单次发布（含组帧）的耗时极值与所在交换号。 */
    uint64_t publish_max_ns;
    uint64_t publish_max_exchange;
    /* 阈值 = 周期 / 100，随部署周期伸缩，记下来免得判据随配置漂移。 */
    uint64_t publish_budget_ns;
    uint64_t publish_over_budget_count;
} emaster_observation_report_t;

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
    /*
     * 整帧缺失单独计数：actual_wkc <= 0（SOEM 的 EC_NOFRAME，一个回帧都没有）
     * 与"帧回来了但计数短"是两种病——前者是链路/调度侧没有回帧，后者是从站不在 OP
     * 导致少计——混在 wkc_* 里既看不出区别，也不能分别设阈值。阈值见策略配置的
     * no_frame_recovery。
     */
    uint64_t wkc_no_frame_count;
    uint64_t wkc_no_frame_consecutive_errors;
    uint64_t wkc_no_frame_max_consecutive_errors;
    uint64_t wkc_no_frame_first_exchange;
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
    /*
     * 停机序言里停止 SDO 观测线程（pthread_join）的耗时。该窗口内不发送任何过程
     * 数据，超过驱动器的同步容差即触发 AL 0x1A 掉出 OP（2026-09-14 台架证据）。
     * 记录成数值才能判断"序言阻塞"这个解释是否留有裕量。
     */
    uint64_t shutdown_observer_join_ns;
    /*
     * 停机序言里过程数据中断的总窗口：最后一条周期帧的发送结束时刻 → 第一条安全停机帧的
     * 发送结束时刻（同一 CLOCK_MONOTONIC 时基）。join 只是这个窗口里的一个分项，两次 AL
     * 快照、观测线程收尾与审计解封同样在其中，只量 join 会低估驱动器真正看到的缺口。
     * 0 表示未测到（没有成功的周期交换，或停机路径整个没走）。
     */
    uint64_t shutdown_prologue_gap_ns;
    /*
     * 周期尾部各段的耗时上限。host_receive_end_ns 在错误计数器读取之后才取，
     * 所以 timing 里的 round_trip 已经把邮箱推进与 3 次 FPRD(0x0300) 算进去了；
     * 只给合计值区分不出"收包慢""邮箱慢"和"FPRD 超时"，而三者的修法不同。
     */
    uint64_t tail_max_receive_ns;
    uint64_t tail_max_mailbox_ns;
    uint64_t tail_max_error_counter_ns;
    /* FPRD(0x0300) 返回 wkc<=0 的次数与首次交换号（每次读取各带 250 µs 超时）。 */
    uint64_t error_counter_read_fail_count;
    uint64_t first_error_counter_read_fail_exchange;
    /*
     * 错误计数器是诊断，不是控制：改为每 N 个周期读一次，且本周期已经在承压
     * （WKC 不符或收包段超常）时整段跳过——那种周期读也读不到，只会再叠最多
     * 3×250 µs 超时。这两个计数把"采样密度"和"因承压跳过"分开记账，否则
     * 报告里看不出计数器是每周期读的还是抽样的。
     */
    uint64_t error_counter_read_attempt_count;
    uint64_t error_counter_skip_count;
    uint64_t first_error_counter_skip_exchange;
    /* 从定时点到周期尾部结束的总耗时超过一个周期的次数与首次交换号。 */
    uint64_t over_budget_cycle_count;
    uint64_t first_over_budget_exchange;
    /*
     * 相邻两次发帧之间的实际间隔（上一次 send_end → 本次 send_end，同一 CLOCK_MONOTONIC
     * 时基）。这是驱动器真正看到的过程数据节奏：over_budget_cycle_count 量的是主站自己
     * 一个周期里花了多久，而驱动器掉出 OP 判的是"两帧之间隔了多久"。恢复路径
     * （emaster_cycle_clock_recover）把 deadline 推到 next 边界时会整周期跳发，
     * 这时主站内部各段计时全都正常，只有这个间隔会露出来。
     *
     * 计数阈值取 1.5 个周期，不能取 1 个周期：DC 相位修正让正常间隔在 1 ms 上下抖动，
     * 按 1 个周期判会把约一半的正常周期算成缺口。max 无阈值，直接取运行期极值。
     */
    uint64_t frame_interval_max_ns;
    uint64_t frame_interval_max_exchange;
    uint64_t frame_interval_gap_count;
    uint64_t first_frame_interval_gap_exchange;
    /* 首次 WKC 不符 / 首次截止超时那两个周期各自的实际帧间隔（0 = 未测到）。 */
    uint64_t first_mismatch_frame_interval_ns;
    uint64_t deadline_miss_frame_interval_ns;
    /*
     * 周期尾部未被已有分段覆盖的那一段：从 receive_end（错误计数器读取之后）到周期末尾
     * 截止检查之前的耗时。中间是 timing 统计、现场入环和逐轴 PDO 审计输出，此前没有
     * 计时点，1 ms 超限里如果有大块时间落在这里，报告无法回答。
     */
    uint64_t tail_uncovered_max_ns;
    uint64_t tail_uncovered_max_exchange;
    uint64_t deadline_miss_tail_uncovered_ns;
    /* 首次 WKC 不符的整体坐标；逐轴现场在 axes[] 的 first_mismatch_* 里。 */
    bool first_mismatch_present;
    uint64_t first_mismatch_exchange;
    int first_mismatch_wkc;
    /* 首次截止超时对应的周期号（0 = 未出现）。 */
    uint64_t first_deadline_missed_exchange;
    emaster_observation_report_t observation;
    emaster_shutdown_prologue_t shutdown_prologue;
    emaster_shutdown_cycle_trace_t shutdown_cycles;
    emaster_observer_stop_trace_t observer_stop;
    size_t thread_schedstat_count;
    emaster_thread_schedstat_t thread_schedstat[EMASTER_THREAD_SCHEDSTAT_CAPACITY];
    emaster_cycle_trace_t cycle_trace;
    emaster_cycle_failure_t first_cycle_failure;
    emaster_runtime_failure_t first_runtime_failure;
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
