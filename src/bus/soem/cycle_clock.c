#define _POSIX_C_SOURCE 200809L

#include "cycle_clock.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

enum
{
    EMASTER_NANOSECONDS_PER_SECOND = 1000000000,
    /* 与 SOEM 官方 ec_sample 的 0.01 和 0.00002 增益完全等价。 */
    EMASTER_DC_PROPORTIONAL_DIVISOR = 100,
    EMASTER_DC_INTEGRAL_DIVISOR = 50000,
    /* 恢复后与恢复过来的那个周期边界之间至少留出的余量，见 recover。 */
    EMASTER_RECOVERY_BOUNDARY_GUARD_NS = 100000
};

static bool timespec_add_ns(struct timespec *value, int64_t nanoseconds)
{
    int64_t seconds;
    int64_t remainder;
    int64_t normalized_nanoseconds;

    if (value == NULL)
    {
        return false;
    }
    seconds = nanoseconds / (int64_t)EMASTER_NANOSECONDS_PER_SECOND;
    remainder = nanoseconds % (int64_t)EMASTER_NANOSECONDS_PER_SECOND;
    normalized_nanoseconds = (int64_t)value->tv_nsec + remainder;
    if (normalized_nanoseconds < 0)
    {
        normalized_nanoseconds += (int64_t)EMASTER_NANOSECONDS_PER_SECOND;
        --seconds;
    }
    else if (normalized_nanoseconds >= (int64_t)EMASTER_NANOSECONDS_PER_SECOND)
    {
        normalized_nanoseconds -= (int64_t)EMASTER_NANOSECONDS_PER_SECOND;
        ++seconds;
    }
    value->tv_sec += (time_t)seconds;
    value->tv_nsec = (long)normalized_nanoseconds;
    return true;
}

static int64_t saturating_add(int64_t left, int64_t right)
{
    if (right > 0 && left > INT64_MAX - right)
    {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right)
    {
        return INT64_MIN;
    }
    return left + right;
}

bool emaster_cycle_clock_init(emaster_cycle_clock_t *clock, uint32_t cycle_ns,
                              uint32_t target_phase_ns)
{
    if (clock == NULL || cycle_ns == 0U || target_phase_ns >= cycle_ns)
    {
        return false;
    }
    memset(clock, 0, sizeof(*clock));
    if (clock_gettime(CLOCK_MONOTONIC, &clock->deadline) != 0)
    {
        return false;
    }
    clock->cycle_ns = cycle_ns;
    clock->target_phase_ns = target_phase_ns;
    clock->initialized = true;
    return true;
}

bool emaster_cycle_clock_wait(emaster_cycle_clock_t *clock)
{
    int64_t interval_ns;
    int result;
    uint64_t now_ns;
    uint64_t deadline_ns;

    if (clock == NULL || !clock->initialized || clock->deadline_missed)
    {
        return false;
    }
    interval_ns = (int64_t)clock->cycle_ns + clock->correction_ns;
    if (interval_ns <= 0 ||
        !emaster_cycle_clock_sample(clock, &now_ns, &deadline_ns) ||
        !timespec_add_ns(&clock->deadline, interval_ns))
    {
        return false;
    }
    do
    {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                 &clock->deadline, NULL);
    } while (result == EINTR);
    return result == 0 &&
           emaster_cycle_clock_sample(clock, &now_ns, &deadline_ns);
}

static bool timespec_to_ns(const struct timespec *value, uint64_t *nanoseconds)
{
    if (value->tv_sec < 0 || value->tv_nsec < 0 ||
        value->tv_nsec >= EMASTER_NANOSECONDS_PER_SECOND ||
        (uint64_t)value->tv_sec >
            (UINT64_MAX - (uint64_t)value->tv_nsec) / UINT64_C(1000000000))
    {
        return false;
    }
    *nanoseconds = (uint64_t)value->tv_sec * UINT64_C(1000000000) +
                   (uint64_t)value->tv_nsec;
    return true;
}

bool emaster_cycle_clock_deadline_ns(const emaster_cycle_clock_t *clock,
                                     uint64_t *deadline_ns)
{
    if (clock == NULL || !clock->initialized || deadline_ns == NULL)
    {
        return false;
    }
    return timespec_to_ns(&clock->deadline, deadline_ns);
}

bool emaster_cycle_clock_sample(emaster_cycle_clock_t *clock,
                                uint64_t *now_ns, uint64_t *deadline_ns)
{
    struct timespec now;
    struct timespec next;
    int64_t interval_ns;

    if (clock == NULL || !clock->initialized || clock->deadline_missed ||
        now_ns == NULL || deadline_ns == NULL)
    {
        return false;
    }
    next = clock->deadline;
    interval_ns = (int64_t)clock->cycle_ns + clock->correction_ns;
    if (interval_ns <= 0 || !timespec_add_ns(&next, interval_ns) ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        !timespec_to_ns(&now, now_ns) || !timespec_to_ns(&next, deadline_ns))
    {
        return false;
    }
    if (*now_ns >= *deadline_ns)
    {
        clock->deadline_missed = true;
        if (clock->consecutive_deadline_misses < UINT32_MAX)
        {
            ++clock->consecutive_deadline_misses;
        }
        return false;
    }
    /* 成功完成一个周期，清除连续超时计数。 */
    clock->consecutive_deadline_misses = 0U;
    return true;
}

bool emaster_cycle_clock_observe_dc(emaster_cycle_clock_t *clock,
                                    int64_t dc_time_ns)
{
    int64_t cycle_ns;
    int64_t phase_ns;
    int64_t correction_limit;

    if (clock == NULL || !clock->initialized || dc_time_ns <= 0)
    {
        return false;
    }
    cycle_ns = (int64_t)clock->cycle_ns;
    phase_ns = (dc_time_ns - (int64_t)clock->target_phase_ns) % cycle_ns;
    if (phase_ns > cycle_ns / INT64_C(2))
    {
        phase_ns -= cycle_ns;
    }
    else if (phase_ns < -cycle_ns / INT64_C(2))
    {
        phase_ns += cycle_ns;
    }

    /*
     * 过程帧目标相位和 Sync0 偏移分别来自运行方案，两者不能在周期时钟中相互替代。
     * 相位偏晚时误差为负，从而缩短下一周期；相位偏早时则延长下一周期。
     */
    clock->phase_error_ns = -phase_ns;
    clock->integral_error_ns = saturating_add(
        clock->integral_error_ns, clock->phase_error_ns);
    clock->correction_ns =
        clock->phase_error_ns / (int64_t)EMASTER_DC_PROPORTIONAL_DIVISOR +
        clock->integral_error_ns / (int64_t)EMASTER_DC_INTEGRAL_DIVISOR;

    /* 异常样本不能把下一周期压缩为零；正常闭环不会触及半周期保护边界。 */
    correction_limit = cycle_ns / INT64_C(2);
    if (clock->correction_ns > correction_limit)
    {
        clock->correction_ns = correction_limit;
    }
    else if (clock->correction_ns < -correction_limit)
    {
        clock->correction_ns = -correction_limit;
    }
    clock->dc_feedback_valid = true;
    return true;
}

int64_t emaster_cycle_clock_phase_error_ns(const emaster_cycle_clock_t *clock)
{
    return clock != NULL && clock->dc_feedback_valid ? clock->phase_error_ns : 0;
}

/*
 * 从超时中恢复：把节拍重新钉回最近的周期边界，并保证下一次发帧就在那儿。
 *
 * 关键在 deadline 的含义。wait() 每轮先按 interval 把 deadline 前移（sample 是在
 * 前移之前判超时的），再睡到前移后的值，所以"下一次实际发帧时刻 = 恢复后留下的
 * deadline + interval"。也就是说 deadline 保存的不是"下次发帧时刻"，而是它的前一个
 * 边界。恢复时必须按这个契约反过来减一个周期，否则下一次发帧会落到 now 之后的
 * 第二个边界上，把一次卡顿平白放大成一个完整周期的过程数据空档。
 *
 * 改动前的算法是"deadline 前推至 now 之后的最近边界"，对着上面的契约看，它比想要的
 * 晚一个周期：迟到 1.5 个周期时，旧算法把下一次发帧放到 3 个周期之后，新算法放在
 * 2 个周期边界上——平白多赔一个周期的过程数据空档。
 * （2026-09-15 曾把 4002632 ns 当成现场证据，那是误读：它等于停机序言缺口
 * shutdown_prologue_gap_ns，即"最后一条周期帧 → 第一条安全停机帧"，不是运行期帧距。）
 *
 * 迟到量不足一个周期时，等到边界最多要赔上接近一个完整周期——那部分不是放大，
 * 是为了不破坏 DC 相位而付出的代价（帧仍然落在同一个 Sync0 之前）。只有当边界
 * 近在眼前（余量不足 guard）时才直接发，牺牲最多 guard 的相位，避免下一次 wait
 * 刚进 sample 就又判一次超限、白丢一个周期。
 *
 * DC 积分误差和相位修正值保留，PI 环在恢复后不从零重建；落点仍在原来的节拍网格上，
 * 因此相位不会被这次恢复带偏。
 */
bool emaster_cycle_clock_recover(emaster_cycle_clock_t *clock)
{
    struct timespec now;
    uint64_t now_ns;
    uint64_t deadline_ns;
    int64_t interval_ns;
    int64_t guard_ns;
    uint64_t late_ns;
    uint64_t remainder_ns;
    int64_t delay_ns;
    int64_t advance_ns;

    if (clock == NULL || !clock->initialized || !clock->deadline_missed)
    {
        return false;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        !timespec_to_ns(&now, &now_ns) ||
        !timespec_to_ns(&clock->deadline, &deadline_ns))
    {
        return false;
    }
    interval_ns = (int64_t)clock->cycle_ns + clock->correction_ns;
    if (interval_ns <= 0)
    {
        return false;
    }
    if (now_ns <= deadline_ns)
    {
        clock->deadline_missed = false;
        return true;
    }
    late_ns = now_ns - deadline_ns;
    if (late_ns > (uint64_t)INT64_MAX)
    {
        return false;
    }
    /*
     * 距离 now 之后第一个边界的时长。落在边界上时余数为零，取模后自然得零，
     * 不需要单独判 `now 恰好在边界上` 这个退化情形——它在 2 ms 抢占配 1 ms 周期
     * 时并不罕见。
     */
    remainder_ns = late_ns % (uint64_t)interval_ns;
    delay_ns = (int64_t)(((uint64_t)interval_ns - remainder_ns) % (uint64_t)interval_ns);
    guard_ns = (int64_t)EMASTER_RECOVERY_BOUNDARY_GUARD_NS;
    if (guard_ns > (int64_t)clock->cycle_ns / INT64_C(4))
    {
        guard_ns = (int64_t)clock->cycle_ns / INT64_C(4);
    }
    if (delay_ns < guard_ns)
    {
        delay_ns = guard_ns;
    }
    /* 下一次发帧在 now + delay，按 wait() 的契约留出它的前一个边界。 */
    advance_ns = (int64_t)late_ns + delay_ns - interval_ns;
    if (advance_ns < 0)
    {
        advance_ns = 0;
    }
    if (!timespec_add_ns(&clock->deadline, advance_ns))
    {
        return false;
    }
    clock->deadline_missed = false;
    return true;
}
