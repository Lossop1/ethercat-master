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
    EMASTER_DC_INTEGRAL_DIVISOR = 50000
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
        return false;
    }
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
