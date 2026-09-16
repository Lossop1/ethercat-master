#ifndef EMASTER_OBSERVATION_FRAME_H
#define EMASTER_OBSERVATION_FRAME_H

/*
 * 观测帧：周期线程每拍发布给外部控制器（神经网络策略）的一次原子快照。
 *
 * 为什么要有这一层。上游策略侧的观测需求与主站已有的 PDO 数据几乎重合，差距不在
 * "采什么"，而在"以什么语义发布"：
 *
 * 1. 策略不是按"最新鲜的状态"训练的。Menlo 刻意按总线轮询顺序分组、让不同关节带
 *    不同新鲜度，delay randomization 是标配。所以观测必须能给出**一整帧同源的快照**，
 *    而不是各轴异步拼出来的最新值——现在 status 响应就是边读边格式化
 *    （session_control.c 的 status 分支），直接喂给策略会有隐蔽的轴间错位。
 * 2. 策略需要**历史窗口**（rl_sar 6 帧 @50 Hz = 120 ms，另一例 10 帧 = 200 ms），
 *    而不是只有"此刻"。
 * 3. 策略频率主流 50 Hz，底层是 200–1000 Hz。所以通道应当是"按客户端频率取最新"，
 *    主站自己不降频。
 *
 * 因此帧里同时带 `cycle` 与 `monotonic_ns`：Δcycle != stride 说明主站跳了拍
 * （截止时间恢复会 continue，那几拍本来就没有过程数据），Δt_mono 给出真实 dt。
 * 缺一个都答不全"有没有断、断了多久"。
 *
 * 单位一律保持设备原始计数（counts），不做浮点换算：ENC/减速比这类静态信息走 INFO
 * 下发一次，客户端自己换算。好处是周期路径上没有浮点、没有单位歧义——这与 topology
 * 响应已经建立的切分方式一致。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 帧内轴数上限。取 16 与 EMASTER_EXTERNAL_TARGET_MAX_AXES /
 * EMASTER_RUNTIME_FAILURE_MAX_AXES 一致——报告与外部目标通道都是这个容量，
 * 观测帧没有理由更小；再大也只是把环形缓冲每槽撑大。
 */
#define EMASTER_OBSERVATION_MAX_AXES 16U

/* 轴级标志。让策略能整轴拒绝对坏数据动作——整个设计里最便宜的安全设施，每轴 4 字节。 */
typedef enum
{
    /* 该轴的 WKC 与期望不符。轴数据仍被解出来了，但不可信。 */
    EMASTER_OBSERVATION_AXIS_FLAG_WKC_MISMATCH = UINT32_C(1) << 0U,
    /* 整帧未回（SOEM 的 EC_NOFRAME，一个回帧都没有）。此位置上全是上一拍的残值。 */
    EMASTER_OBSERVATION_AXIS_FLAG_WHOLE_FRAME_MISSING = UINT32_C(1) << 1U,
    /* 该轴的 PDO 解码不完整：映射里的字段没能全部解出。见 cia_process_image 的返回约定。 */
    EMASTER_OBSERVATION_AXIS_FLAG_DECODE_INCOMPLETE = UINT32_C(1) << 2U,
    /* 该轴已被隔离（安全停机后不再输出）。位置仍然是真实反馈，目标值不再是意图。 */
    EMASTER_OBSERVATION_AXIS_FLAG_ISOLATED = UINT32_C(1) << 3U,
    /*
     * 本拍没有写位置目标（例如已进入安全停机、输出被冻结）。此时
     * target_position 保持上一拍的值，**不要当成新目标**。
     */
    EMASTER_OBSERVATION_AXIS_FLAG_TARGET_UNKNOWN = UINT32_C(1) << 4U,
    /*
     * 这套 PDO 映射里没有映射速度反馈（606Ch），actual_velocity 不是量测值。
     *
     * 为什么必须单独标出来而不是留 0：固定 PDO 集合只映射 6041h/6064h，而动态集合
     * 额外映射 606Ch/6077h。同一份代码在两种部署下都会跑，缺字段时留 0 会让"这套
     * 部署不提供速度"和"速度确实是 0"变成同一个值——策略拿它训练就是拿常量当真值。
     * 这是部署属性，在配置期就已知，所以这里标一次即可，不需要每拍重新判断。
     */
    EMASTER_OBSERVATION_AXIS_FLAG_VELOCITY_UNAVAILABLE = UINT32_C(1) << 5U,
    /* 同理，6077h 力矩反馈未映射。 */
    EMASTER_OBSERVATION_AXIS_FLAG_TORQUE_UNAVAILABLE = UINT32_C(1) << 6U
} emaster_observation_axis_flag_t;

/* 帧级标志。 */
typedef enum
{
    /* 本拍至少一个轴 WKC 不符。 */
    EMASTER_OBSERVATION_FRAME_FLAG_WKC_MISMATCH = UINT32_C(1) << 0U,
    /* 本拍整帧未回。此时所有轴的数据都是残值，策略应当整帧丢弃。 */
    EMASTER_OBSERVATION_FRAME_FLAG_WHOLE_FRAME_MISSING = UINT32_C(1) << 1U,
    /* 本拍错过了截止时间。数据可能仍然有效（主站本拍已完成交换），但节拍不可靠。 */
    EMASTER_OBSERVATION_FRAME_FLAG_DEADLINE_MISSED = UINT32_C(1) << 2U,
    /* 相对上一帧，cycle 的增量不等于 stride：主站中间跳了拍。缺口拍数 =
     * (cycle - 上一帧 cycle - stride) / stride。 */
    EMASTER_OBSERVATION_FRAME_FLAG_CYCLE_GAP = UINT32_C(1) << 3U
} emaster_observation_frame_flag_t;

/*
 * 单轴观测。
 *
 * `target_position` 是主站**本拍实际下发**的目标（也就是输出镜像里的值），不是上位机
 * 通过命令 socket 请求的目标——两者在限速、夹紧、安全门拦截之后会分叉，策略需要知道
 * 自己真正被执行的意图是哪个。本拍没写目标时置
 * EMASTER_OBSERVATION_AXIS_FLAG_TARGET_UNKNOWN。
 */
typedef struct
{
    int32_t  actual_position;
    int32_t  target_position;
    int32_t  actual_velocity;
    int32_t  actual_torque;
    uint16_t status_word;
    uint16_t control_word;
    uint32_t flags;
} emaster_observation_axis_t;

/*
 * 一帧观测。
 *
 * `publish_index` 是环形缓冲的发布序号（从 0 起单调递增），与 `cycle` 不同：
 * cycle 会因为截止时间恢复而跳号，publish_index 只数"发布了几帧"。读侧的覆盖
 * 判定与撕裂校验都用它，不用 cycle。
 *
 * `stride`（周期跨度语义，通常为 1，用来解释 cycle 的增量）不在这里：它是部署常量，
 * 走 INFO 下发一次即可，每拍重复携带只是白占带宽。
 */
typedef struct
{
    uint64_t publish_index;
    uint64_t cycle;
    uint64_t monotonic_ns;
    uint64_t deadline_ns;
    uint64_t frame_interval_ns;
    int32_t  wkc;
    uint16_t axis_count;
    uint16_t reserved;
    uint32_t flags;
    emaster_observation_axis_t axes[EMASTER_OBSERVATION_MAX_AXES];
} emaster_observation_frame_t;

/* 空帧，供 memset 语义之外的确定性初始化（含 reserved，避免不可复现的报告字节）。 */
void emaster_observation_frame_clear(emaster_observation_frame_t *frame, uint16_t axis_count);

#endif /* EMASTER_OBSERVATION_FRAME_H */
