/*
 * 周期时钟恢复语义的回归测试：一次卡顿不得被放大成更长的过程数据空档。
 *
 * 现场来源（2026-09-15 带电负载运行，报告 archive/20260915-135103-run-load60.json）：
 * 周期线程被 SCHED_FIFO 90 的压载每 200 ms 抢占 2 ms，2 ms 的停顿最终表现为
 * frame_interval_ns = 4002632 —— 主站自己多赔了将近一个完整周期，驱动器正是在
 * 那一帧掉出 OP（wkc 由 12 掉到 6，四轴 AL 全部 0x001A）。
 *
 * 测试复刻的是交换循环的真实调用次序，而不是直接调 recover：
 *   wait() 成功 -> 记录本周期发包时刻 -> 尾部 sample() 判超时 -> recover()
 * 并故意把"尾部工作"拖到超出 2.2 个周期，制造出与现场同量级的停顿。
 *
 * 断言的性质（不含任何魔数）：
 *   1. 下一次发包距上一次不超过 停顿时长 + 一个周期 —— 恢复最多只该赔一个周期，
 *      且这一个周期是"等回节拍边界"的代价，不是放大；
 *   2. 该间隔是周期的整数倍 —— 落点仍在本来的 DC 节拍网格上，相位没有被带偏。
 *
 * 编译与运行（本机 MinGW 与 Pi 都适用；不依赖 SOEM）：
 *   gcc -std=c11 -O2 -Isrc -o tmp/test_recovery_gap \
 *       tests/unit/cycle_clock/test_recovery_gap.c src/bus/soem/cycle_clock.c
 *   ./tmp/test_recovery_gap
 */
#define _POSIX_C_SOURCE 200809L

#include "bus/soem/cycle_clock.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

enum
{
    CYCLE_NS = 1000000,
    TARGET_PHASE_NS = 200000,
    /* 制造停顿的时长：现场是 2 ms 抢占 + 自身尾部开销，取 2.2 个周期。 */
    STALL_NS = 2200000,
    /* 停顿前的正常周期数，用来确认基线稳定。 */
    WARMUP_CYCLES = 5
};

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/* 忙等到绝对时刻：模拟"被更高优先级抢走"，nanosleep 在这里会睡过头。 */
static void spin_until(uint64_t target_ns)
{
    while (monotonic_ns() < target_ns)
    {
    }
}

int main(void)
{
    emaster_cycle_clock_t clock;
    uint64_t last_send_ns = 0U;
    uint64_t gap_ns = 0U;
    uint64_t stall_start_ns = 0U;
    uint32_t wake_late_max_ns = 0U;
    int failures = 0;

    if (!emaster_cycle_clock_init(&clock, CYCLE_NS, TARGET_PHASE_NS))
    {
        fprintf(stderr, "时钟初始化失败\n");
        return 1;
    }

    for (int cycle = 0; cycle < WARMUP_CYCLES + 2; ++cycle)
    {
        uint64_t deadline_ns;
        uint64_t now_ns;
        uint64_t next_ns;
        uint64_t send_ns;
        /* 停顿安排在 WARMUP_CYCLES 发包之后，因此它拉长的是下一个周期的帧距。 */
        bool after_stall = (cycle == WARMUP_CYCLES + 1);

        if (!emaster_cycle_clock_wait(&clock))
        {
            fprintf(stderr, "周期 %d 的 wait 失败\n", cycle);
            return 1;
        }
        if (!emaster_cycle_clock_deadline_ns(&clock, &deadline_ns))
        {
            fprintf(stderr, "读取 deadline 失败\n");
            return 1;
        }
        send_ns = monotonic_ns();
        if (last_send_ns != 0U)
        {
            uint64_t gap = send_ns - last_send_ns;

            if (after_stall)
            {
                gap_ns = gap;
            }
            else if (gap > CYCLE_NS + 300000U)
            {
                fprintf(stderr, "基线不稳：第 %d 周期帧距 %" PRIu64 " ns\n", cycle, gap);
                ++failures;
            }
        }
        last_send_ns = send_ns;

        {
            uint32_t late_ns = (uint32_t)(send_ns - deadline_ns);

            if (late_ns > wake_late_max_ns)
            {
                wake_late_max_ns = late_ns;
            }
        }

        if (cycle == WARMUP_CYCLES)
        {
            /* 现场里这 2 ms 抢占发生在周期尾部；这里等价地把它放在发包之后。 */
            stall_start_ns = send_ns;
            spin_until(send_ns + STALL_NS);
        }

        if (!emaster_cycle_clock_sample(&clock, &now_ns, &next_ns))
        {
            if (!clock.deadline_missed)
            {
                fprintf(stderr, "第 %d 周期 sample 非超时失败\n", cycle);
                return 1;
            }
            if (cycle != WARMUP_CYCLES)
            {
                fprintf(stderr, "第 %d 周期意外超时（停顿只安排在 %d）\n",
                        cycle, WARMUP_CYCLES);
                return 1;
            }
            if (!emaster_cycle_clock_recover(&clock))
            {
                fprintf(stderr, "恢复失败\n");
                return 1;
            }
        }
        else if (cycle == WARMUP_CYCLES)
        {
            fprintf(stderr, "停顿 %d ns 没有触发超时，测试无效\n", STALL_NS);
            return 1;
        }
    }

    printf("停顿起点 %" PRIu64 " ns，停顿时长 %d ns\n", stall_start_ns, STALL_NS);
    printf("唤醒偏迟峰值 %" PRIu32 " ns\n", wake_late_max_ns);
    printf("停顿后的帧距 %" PRIu64 " ns（周期 %d ns）\n", gap_ns, CYCLE_NS);

    if (gap_ns == 0U)
    {
        fprintf(stderr, "没有取到停顿后的帧距\n");
        return 1;
    }
    /* 性质 1：放大不得超过一个周期。 */
    if (gap_ns > STALL_NS + CYCLE_NS)
    {
        fprintf(stderr, "帧距 %" PRIu64 " ns 超过 停顿时长+一个周期 = %d ns\n",
                gap_ns, STALL_NS + CYCLE_NS);
        ++failures;
    }
    /* 性质 2：落点仍在原节拍网格上。 */
    if (gap_ns % CYCLE_NS > 100000U && gap_ns % CYCLE_NS < (uint64_t)CYCLE_NS - 100000U)
    {
        fprintf(stderr, "帧距 %" PRIu64 " ns 不是周期的整数倍，相位被带偏\n", gap_ns);
        ++failures;
    }

    if (failures != 0)
    {
        printf("失败：%d 项\n", failures);
        return 1;
    }
    printf("通过\n");
    return 0;
}
