#ifndef EMASTER_OBSERVATION_SLOW_H
#define EMASTER_OBSERVATION_SLOW_H

/*
 * 慢速档观测：走 SDO 轮询拿到的那些量（电流、母线电压、温度、电机速度、错误码）。
 *
 * 这些对象没有映射进 TxPDO，只能由观测线程以 ~20 Hz 通过 SDO 逐个读。它们的新鲜度
 * 天然与周期不同，所以**不能**混进每拍的快速帧里冒充同源数据——那会让策略以为
 * 电流和位置是同一时刻的。
 *
 * 发布粒度是"观测线程一轮迭代一次"，不是每轴一次。这正是"一轮里所有轴同源"的保证：
 * 一轮里读到的轴间时间偏差被如实保留在一个快照里，而不是被逐轴发布抹平。
 *
 * 用 seqlock 而不是互斥锁：读者（停机后回填报告的 flush_slow）宁可重试两次读到一个
 * 完整快照，也不该让观测线程在 SDO 读的间隙去抢锁。写者不与读者同步，只做两次序号
 * 存 + 一次屏障。
 *
 * 读者必须是单线程——这不是限制，是 seqlock 的固有性质：多个读者之间没有互斥，
 * 各自重试互不干扰，但同一个读结果缓冲不能共享。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "emaster/observation/frame.h"

/* 单轴的慢速观测。单位保持设备原始单位（与快速帧同理，换算交给客户端）。 */
typedef struct
{
    /* 本轮该轴有没有真正读到。false 时其余字段保持上一轮的值，不得当作新数据。 */
    bool     valid;
    /*
     * 最后一次读到这一轴的时刻（CLOCK_MONOTONIC，纳秒），0 = 从没读到过。
     *
     * 与 valid 分开，因为两者回答的不是同一个问题，而报告要的是这一个：
     *   valid        —— "这一轴在最后一轮里读到没有"（当轮的性质）
     *   last_read_ns —— "最后一次读到是什么时候"（跨轮保留的性质）
     *
     * 停机信号落在一轮中途时，被截断的永远是最后一轮，而值本身留在快照里
     * （写样本只在校验成功时改字段）。拿 valid 当"有没有值"用，就会把"上一轮
     * 读到的 42.0 °C"印成 0——"没读到"与"读到 0"从此不可区分（P11.8）。
     */
    uint64_t last_read_ns;
    int32_t  actual_current;
    int32_t  dc_link_voltage;
    int32_t  mosfet_temperature;
    int32_t  motor_temperature;
    int32_t  motor_speed;
    int32_t  speed_command;
    uint16_t error_code;
} emaster_observation_slow_axis_t;

/* 一次快照的载荷。发布与读取都以整块为单位，不含序号。 */
typedef struct
{
    /* 发布时的主站周期号，供消费者把这批慢速量与快速帧对齐。 */
    uint64_t cycle;
    uint64_t monotonic_ns;
    uint32_t axis_count;
    /* 观测线程累计完成的轮次，用来判断慢速档是否还在推进。 */
    uint32_t read_count;
    /* 位 i 置位表示 axes[i].valid。上限 64 轴。 */
    uint64_t valid_mask;
    emaster_observation_slow_axis_t axes[EMASTER_OBSERVATION_MAX_AXES];
} emaster_observation_slow_state_t;

/* 带序号的存储。读者不应直接访问 state，必须走 read()。 */
typedef struct
{
    _Atomic uint32_t sequence;
    uint32_t _pad;
    emaster_observation_slow_state_t state;
} emaster_observation_slow_t;

/* 归零。必须在任何线程启动前调用。 */
void emaster_observation_slow_init(emaster_observation_slow_t *snapshot);

/*
 * 发布一次快照。**观测线程专用**（非实时），但被实时线程调用也是安全的：
 * 不取锁、不分配、不阻塞，只有两次序号存和一次屏障。
 * *state 里超过 EMASTER_OBSERVATION_MAX_AXES 的 axis_count 会被截断；
 * valid_mask 由本函数按 axes[].valid 重新算，调用者填的值会被覆盖。
 */
void emaster_observation_slow_publish(emaster_observation_slow_t *snapshot,
                                      const emaster_observation_slow_state_t *state);

/*
 * 读到一份自洽的快照。有界重试，两次都没读到一致版本时返回 false 且不写 *out
 * ——宁可告诉调用者"这次没读到"，也不要给一份半旧的混合数据。
 */
bool emaster_observation_slow_read(const emaster_observation_slow_t *snapshot,
                                   emaster_observation_slow_state_t *out);

/*
 * 取这一轴读数的"年龄"：读到过就返回 true 并写 *out_age_ns = now_ns - last_read_ns，
 * 从没读到过（或轴号越界）就返回 false 且**不写** *out_age_ns。
 *
 * 这是"报告里该印读数还是印「—」"的唯一判据，抽成纯函数是为了它能离线被测：
 * 值该不该显示、显示出来是多久以前的，与总线、线程、时钟都无关。
 * now_ns 与 last_read_ns 必须同源（CLOCK_MONOTONIC）；now 比 last_read 小
 * （时钟异常）时年龄取 0，不返回负数。
 */
bool emaster_observation_slow_axis_age(const emaster_observation_slow_state_t *state,
                                       uint32_t axis_index, uint64_t now_ns,
                                       uint64_t *out_age_ns);

#endif /* EMASTER_OBSERVATION_SLOW_H */
