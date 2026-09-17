#ifndef EMASTER_BUS_SOEM_SESSION_INTERNAL_H
#define EMASTER_BUS_SOEM_SESSION_INTERNAL_H

#include "cia_process_image.h"
#include "cycle_clock.h"
#include "emaster/bus/command_server.h"
#include "emaster/bus/control_session.h"
#include "emaster/config/error_recovery_config.h"
#include "emaster/cyclic/window.h"
#include "emaster/multiaxis/coordinator.h"
#include "emaster/motion/velocity_profile.h"
#include "emaster/observation/ring.h"
#include "emaster/observation/server.h"
#include "emaster/observation/slow.h"
#include "emaster/safety/gate.h"
#include "session_observer.h"

/*
 * 单次会话拥有全部总线资源和周期存储。入口负责分配和释放，配置、交换、控制、
 * 退出各阶段只借用这里的资源；这些内部类型不得传入 CiA 402 或轨迹模块。
 *
 * SOEM 访问不是"只由会话线程执行"的——P4.3 的观测线程（session_observer_thread.c
 * 的 ecx_SDOread）与周期线程共用同一个 context 和 socket。之所以成立，靠的是
 * SOEM 的 Linux 移植自带的串行化，不是本项目的约定：
 *
 *   - 三把互斥锁 getindex_mutex / tx_mutex / rx_mutex 在 external/SOEM/oshw/linux/
 *     nicdrv.c:128-132 用 PTHREAD_PRIO_INHERIT 初始化，每次收发各取放一次
 *     （取 :223/:312/:404，放 :247/:326/:458）。持有时间是一次系统调用，有界；
 *     周期线程可能短暂等在这里，这是实时路径上的一处有界等待。
 *   - 收包是非阻塞的（nicdrv.c:420），且按帧内索引分流：收到不是自己要的帧时，
 *     存进对应索引的缓冲区并置 EC_BUF_RCVD（nicdrv.c:436-447），不是丢掉。所以
 *     观测线程**不会**把过程数据帧取走。
 *
 * 线程归属（与 scripts/checks/soem_call_sites.sh 的 ALLOWED 一一对应，那张名单
 * 是这条约束的可执行检查，改动本段时必须同步）：
 *
 *   周期线程（RT）      session_exchange.c
 *   观测线程（P4.3）    session_observer_thread.c
 *   会话线程（周期外）  session_setup.c / session_start.c / session_shutdown.c
 *                       dc_prepare.c / preop_probe.c / session_observer.c
 *   多线程共用          soem_common.c（薄封装，线程由调用方决定）
 *
 * 另注：周期里的 ecx_mbxhandler 调用目前是空转——只有 ecx_slavembxcyclic() 把
 * 从站设成循环邮箱模式后它才真的收发，而全仓没有一处调用它。观察线程的邮箱
 * 往返走的是 SOEM 的直接阻塞路径，在它自己的线程里完成。
 */
typedef struct {
    const emaster_session_plan_t *plan;
    emaster_control_session_axis_result_t *axes;
    emaster_control_session_report_t *report;
    ecx_contextt context;
    pthread_mutex_t error_ring_mutex;
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
    /* 协调器的最后一次判定（emaster_multiaxis_status_t）。故障锁存时记入报告现场：
     * 非零说明是协调器拒绝了整帧，为零说明故障出在其后的健康检查或使能确认。
     * -1 表示协调器还没被调用过（配置或使能前就失败了），不是"通过"。 */
    int last_coordinator_status;
    emaster_cycle_clock_t clock;
    uint8_t *io_map;
    uint64_t exchange;
    /* 停机序言缺口的起点：最后一条周期帧的发送结束时刻（CLOCK_MONOTONIC ns）。
     * 在停机入口抓取，第一条安全停机帧发出后与终点相减，得到驱动器实际经历的数据中断窗口。 */
    uint64_t shutdown_prologue_start_ns;
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
    /* 0 表示调用者未启用单步限幅；非零时拒绝相邻目标差超过此值的命令。 */
    uint64_t position_target_max_step_counts;
    /* 至少已成功提交一条外部目标后置 true；用于单步限幅的基准有效性判断。 */
    bool position_target_committed;
    /* 错误恢复策略配置：从部署配置加载，与业务逻辑解耦 */
    const emaster_error_recovery_policy_t *error_recovery_policy;
    /* WKC 错误恢复计数器。连续段用联合计数，累计量改由下面两个滑动窗口承担。 */
    uint64_t wkc_consecutive_errors;
    /*
     * 整帧缺失（actual_wkc <= 0）单独一套计数：它与"帧回来了但短"成因不同
     * （链路/调度 vs 从站不在 OP），阈值也各自配置，混用会让两类故障互相掩盖。
     */
    uint64_t no_frame_consecutive_errors;
    /*
     * 累计阈值的滑动窗口计数，短帧与整帧缺失各一个。放在这里而不是本地静态变量：
     * 阈值判定分布在每次交换里，窗口必须跨周期存在，且必须随会话重建而清零。
     * 只有真出现错误时才被写入，因此健康周期上这两个结构一分钱不花。
     */
    emaster_cyclic_window_t wkc_window;
    emaster_cyclic_window_t no_frame_window;
    /*
     * 帧距仪表：上一次发帧结束时刻，以及它与本次之间的间隔。
     * 间隔在发帧后立刻算出，现场入环时直接取用，避免跨周期取值的错位。
     */
    uint64_t last_send_end_ns;
    bool last_send_end_valid;
    uint64_t frame_interval_ns;
    /* 实时命令服务器：运行期间接收外部命令（可选） */
    emaster_command_server_t *command_server;
    /* 外部目标双缓冲区：由上层注入，会话不拥有内存。 */
    emaster_external_target_buffer_t *external_target_buffer;
    /* P4.3: SDO 慢速观测线程 */
    pthread_t observer_thread;
    bool observer_thread_created;
    /* 跨线程停止标志：观测线程每次邮箱读之前都查它，停机序言的 join 延迟因此有界。
     * 用 volatile 而不是普通 bool，避免这个检查被优化掉后 join 又等满一次完整迭代。 */
    volatile bool observer_running;
    /* 观测线程是否已经跑完线程函数。只负责"先看一眼"——真正回收线程仍然是 pthread_join，
     * 所以这里不需要比 running 更强的同步。有了它，停机序言才能把 join 拆成
     * "置标志 → 每周期看一眼 → 回收"三段，中间照常发帧。 */
    volatile bool observer_exited;
    /*
     * 慢速遥测快照（SDO 轮询拿到的电流/电压/温度/速度/错误码）。
     *
     * 取代原先"观测线程加锁写 session->axes[]、周期线程加锁抄一遍"的做法：那把锁
     * 没有 PRIO_INHERIT，而持锁的观测线程会在锁内做 fprintf 和邮箱往返，周期线程等
     * 它就成了实时路径上唯一一处**无界**等待。现在写者（观测线程）只做两次序号存
     * 和一次屏障，读者（停机后回填报告的 flush_slow、处理 status 命令时的周期线程）
     * 有界重试两次，写者永不等待。
     *
     * 内联而不是堆分配：整个快照约 600 B，而本结构本来就是栈局部变量；对比之下
     * observation_ring 有 108 KiB，那个才必须上堆。内联顺带消掉了"什么时候分配、
     * 谁来释放、线程启动前有没有初始化"这一整类问题。
     *
     * 注意它与 observation_ring 的开关**无关**：报告里的这几列一直都有，不能因为
     * 观测通道开关关着就消失。
     */
    emaster_observation_slow_t observation_slow;
    /*
     * 观测通道的环形缓冲。开关关闭时为 NULL，周期路径上只有一次指针判空。
     *
     * 不在本结构里内联而用指针：缓冲是 256 槽 × 432 B ≈ 108 KiB，而本结构是
     * emaster_soem_control_session() 的栈局部变量。内联进来会让单个会话的栈占用
     * 再涨一百多 KB——在默认 8 MB 的主线程栈上不会立刻出事，但这是个没人会再注意到
     * 的增长。堆上一次分配，生命周期与会话相同。
     */
    emaster_observation_ring_t *observation_ring;
    /*
     * 只读套接字服务器。它的监听线程在周期线程之外读 observation_ring，
     * 因此**必须先销毁它再释放缓冲**，见 observation_close。
     */
    emaster_observation_server_t *observation_server;
    /* 上一帧的周期号，用于判定本拍相对上一帧是不是跳了拍（CYCLE_GAP）。 */
    uint64_t observation_last_cycle;
    bool observation_last_cycle_valid;
} emaster_soem_session_t;

/*
 * 建立观测通道的环形缓冲。返回 false 时通道未启用（开关关闭或分配失败），
 * 会话照常运行——观测是旁路，不能因为它启动失败就让控制回路停摆。
 */
bool emaster_soem_session_observation_open(emaster_soem_session_t *session);

/* 释放环形缓冲并把报告字段归位。可重复调用。 */
void emaster_soem_session_observation_close(emaster_soem_session_t *session);

/*
 * 发布本拍观测。**周期线程专用**，wait-free、不取锁、不分配。
 *
 * 必须在轴解码循环之后调用：此处 axes[] 里的反馈是本拍刚解出来的，而
 * control_word/target_position 是本拍**之前**由协调器写下的，也就是此刻真正在
 * 总线上生效的那一份输出。这个配对不是巧合——反馈落在这拍、指令是上一拍算的，
 * 而上一拍算的指令正好在这一拍驱动着电机。策略要的"看着这个状态做了那个动作"
 * 就是这个配对；用本拍还没算出来的指令去配本拍的状态反而不成立。
 */
void emaster_soem_session_observation_publish(emaster_soem_session_t *session,
                                              uint64_t now_ns,
                                              uint64_t deadline_ns);

/*
 * 第 axis_index 个配置轴对应总线上哪个从站（SOEM slavelist 下标，1 基）。
 *
 * 别再写 axis_index + 1U。那只在"接线顺序与配置顺序一致"时才碰巧正确，而
 * emaster_soem_session_map_topology 是按身份（vendor/product/revision）在发现表里
 * 找匹配的，本来就允许从站跳跃或与配置顺序不同。映射结果存在 axes[].position，
 * 它是这个问题的唯一真相。位置在会话配置期写一次，此后只读，观测线程读它安全。
 *
 * 前提：仅在 emaster_soem_session_configure 走完映射之后调用；映射失败会让会话在
 * 配置期就终止，所以此后 position 必为非零的真实总线位置（0 是广播/邮箱从站）。
 */
static inline uint16_t emaster_soem_session_axis_slave(
    const emaster_soem_session_t *session,
    size_t axis_index)
{
    return session->axes[axis_index].position;
}

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
/* 周期超时处理：先记账（每轴 deadline_missed_count），再按容错策略尝试重新对齐。
 * 交换函数末尾与循环顶部的同名检查是同一个条件，必须共用这两个入口，否则同一次
 * 超时会因检出位置不同而结局相反（一处可恢复、另一处直接终止且不留现场）。 */
void emaster_soem_session_note_deadline_missed(emaster_soem_session_t *session);
bool emaster_soem_session_try_deadline_recovery(emaster_soem_session_t *session);
/* 环境变量开关：只认写明的几种真值（1/on/true/yes，大小写不敏感）。
 * 停机序言的快路径与周期内首次不符的 AL 读取都用它做对照实验的开关，
 * 因此放在这里共用一份，不各自复制。 */
bool emaster_soem_env_flag_enabled(const char *name);
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

/* P4.5: 线程安全的错误环访问包装。加锁后调用 ecx_poperror()，保护并发访问。
 * 返回值：true = 成功弹出错误，false = 错误环为空或互斥锁失败。 */
bool emaster_soem_pop_error_safe(emaster_soem_session_t *session, ec_errort *error);

/* P9.2: 取空错误环并逐条记进报告（soem_error_count / soem_errors[]）。
 * 周期回路每拍调一次，停机收尾再调一次；空环时不取锁，只比一次头尾。
 * 没有它，环里唯一带时间戳和 abort 码的那条记录没人在运行期取用。 */
void emaster_soem_drain_errors(emaster_soem_session_t *session);

/* P4.3: SDO 慢速观测线程管理 */
bool emaster_soem_session_start_observer(emaster_soem_session_t *session);
void emaster_soem_session_stop_observer(emaster_soem_session_t *session);
/* 三段式停机（停机序言融入周期时用）：置标志不阻塞 / 只看一眼 / 回收线程。
 * stop_observer = request + reap；只有在 observer_exited 为真之后 reap 才不阻塞。 */
void emaster_soem_session_request_observer_stop(emaster_soem_session_t *session);
bool emaster_soem_session_observer_exited(const emaster_soem_session_t *session);
void emaster_soem_session_reap_observer(emaster_soem_session_t *session);

/*
 * 把慢速快照回填进报告字段。**必须在观测线程 join 之后调用**（reap 里就是那一点）：
 * 回填读的是快照，而报告序列化读的是 session->axes[]，这一步是两者之间唯一的桥。
 *
 * 只在停机时做一次是有意的。原先那条路（周期线程每拍加锁抄一遍）建立了"报告字段
 * 始终是最新值"的假象，代价是实时路径上的一把无继承优先级锁；而报告本身是停机后
 * 才落盘的，运行期的中间值没有任何消费者。
 */
void emaster_soem_session_observation_flush_slow(emaster_soem_session_t *session);

/* 拓扑映射：动态从站发现和轴匹配 */
typedef struct {
    uint16_t bus_position;
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    bool assigned;
} emaster_discovered_slave_t;

bool emaster_soem_session_map_topology(
    emaster_soem_session_t *session,
    int slave_count,
    emaster_discovered_slave_t *discovered);

#endif
