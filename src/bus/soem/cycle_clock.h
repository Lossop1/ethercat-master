#ifndef EMASTER_BUS_SOEM_CYCLE_CLOCK_H
#define EMASTER_BUS_SOEM_CYCLE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * 周期时钟只负责主机唤醒节拍和 DC 相位校正，不访问 EtherCAT 总线，也不解释设备状态。
 * 所有状态属于一次控制会话，重新建立会话时不会继承上次运行的积分误差。
 */
typedef struct
{
    struct timespec deadline;
    uint32_t cycle_ns;
    uint32_t target_phase_ns;
    int64_t integral_error_ns;
    int64_t correction_ns;
    int64_t phase_error_ns;
    uint32_t consecutive_deadline_misses;
    bool initialized;
    bool dc_feedback_valid;
    bool deadline_missed;
} emaster_cycle_clock_t;

/* 以当前单调时钟为起点建立周期；首次等待发生在一个完整周期之后。 */
bool emaster_cycle_clock_init(emaster_cycle_clock_t *clock, uint32_t cycle_ns,
                              uint32_t target_phase_ns);

/* 按绝对期限等待下一周期，避免每次相对休眠累积主机执行时间。 */
bool emaster_cycle_clock_wait(emaster_cycle_clock_t *clock);

/* 返回刚刚等待到的主站周期截止点，供时序统计将主机与 DC 时间分开记录。 */
bool emaster_cycle_clock_deadline_ns(const emaster_cycle_clock_t *clock,
                                     uint64_t *deadline_ns);

/* 当前帧必须在下一发送时刻前完成；超时锁存到重新初始化，禁止追赶旧帧。 */
bool emaster_cycle_clock_sample(emaster_cycle_clock_t *clock,
                                uint64_t *now_ns, uint64_t *deadline_ns);

/*
 * 使用 SOEM 最近一次过程数据接收得到的 DC 系统时间修正下一周期。
 * 返回 false 表示样本无效，调用者可以继续使用未校正的标称周期。
 */
bool emaster_cycle_clock_observe_dc(emaster_cycle_clock_t *clock,
                                    int64_t dc_time_ns);

/* 返回最近一次有效 DC 样本相对目标相位的有符号误差。 */
int64_t emaster_cycle_clock_phase_error_ns(const emaster_cycle_clock_t *clock);

/*
 * 从死区超时状态中恢复：清除 deadline_missed，将 deadline 前推至最近的未来周期边界。
 * 仅在调用者确认连续超次数未超阈值后调用；consecutive_deadline_misses 保持不变，
 * 由调用者在后续成功周期中通过 emaster_cycle_clock_wait 返回 true 时自动清零。
 */
bool emaster_cycle_clock_recover(emaster_cycle_clock_t *clock);

#endif
