/*
 * 外部目标合理性判据：|目标 - 实际位置| 与逐轴上限的比较。
 *
 * 这是外部目标路径上**唯一**保留的判据（原先的软限位、力矩、单步三条已删）。它必须
 * 同时做到两件相反的事：放过正常跟随，拦住明显不合理的值。判据本身是纯函数，所以
 * 这两件事都能在这里钉死，不用上台架。
 *
 * 红了先看这里再改 include/emaster/motion/target_guard.h：判据是距离而不是增量、
 * 且逐轴取限，这两点是它存在的理由，改掉就等于把删掉的单步闸换了个名字请回来。
 */

#include <stdio.h>

#include "emaster/motion/target_guard.h"

static int failures = 0;

static void check_result(const char *name,
                         emaster_target_guard_result_t actual,
                         emaster_target_guard_result_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %d, got %d\n", name, (int)expected, (int)actual);
        ++failures;
    }
}

static void check_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %llu, got %llu\n", name,
                (unsigned long long)expected, (unsigned long long)actual);
        ++failures;
    }
}

int main(void)
{
    /* 限内：偏移 100，上限 200 —— 放过。 */
    {
        const int32_t targets[2] = {1100, -1100};
        const int32_t actual[2] = {1000, -1000};
        const uint64_t limits[2] = {200U, 200U};
        size_t axis = 99U;
        uint64_t offset = 0U;

        check_result("限内",
                     emaster_target_guard_check(targets, actual, limits, 2U, &axis, &offset),
                     EMASTER_TARGET_GUARD_OK);
        check_u64("限内不改出参", offset, 0U);
    }

    /* 超限：偏移 300，上限 200 —— 拒，并报出是第 0 轴、偏移 300。 */
    {
        const int32_t targets[2] = {1300, -1000};
        const int32_t actual[2] = {1000, -1000};
        const uint64_t limits[2] = {200U, 200U};
        size_t axis = 99U;
        uint64_t offset = 0U;

        check_result("超限",
                     emaster_target_guard_check(targets, actual, limits, 2U, &axis, &offset),
                     EMASTER_TARGET_GUARD_TOO_FAR);
        if (axis != 0U)
        {
            fprintf(stderr, "FAIL 超限轴号: expected 0, got %zu\n", axis);
            ++failures;
        }
        check_u64("超限偏移", offset, 300U);
    }

    /* 边界：偏移正好等于上限——判据是"大于"，所以放过。 */
    {
        const int32_t targets[1] = {1200};
        const int32_t actual[1] = {1000};
        const uint64_t limits[1] = {200U};

        check_result("偏移等于上限",
                     emaster_target_guard_check(targets, actual, limits, 1U, NULL, NULL),
                     EMASTER_TARGET_GUARD_OK);
    }

    /*
     * 逐轴取限。台架第 8、12 席是 25:1、其余 28:1，同一个角度折算出来的 counts 不同。
     * 这里复现同一件事：两轴偏移量不同（200 与 250），限也不同，于是**同一批数据**
     * 只要把限换到另一轴，结论就翻转。若哪天又退回"首轴统一"（限取自 axes[0] 再套给
     * 所有轴），两组会得到同一个结论，这里必红。
     */
    {
        const int32_t targets[2] = {1200, 1250};
        const int32_t actual[2] = {1000, 1000};

        /* 限 200 给偏移 200 的轴（正好等于，放过）、限 300 给偏移 250 的轴（放过）。 */
        {
            const uint64_t limits[2] = {200U, 300U};
            check_result("逐轴取限（各归各，放过）",
                         emaster_target_guard_check(targets, actual, limits, 2U, NULL, NULL),
                         EMASTER_TARGET_GUARD_OK);
        }

        /* 两个限对调：偏移 250 的轴落到 200 的限上，越界；报出的必须是第 1 轴。 */
        {
            const uint64_t limits[2] = {300U, 200U};
            size_t axis = 99U;
            uint64_t offset = 0U;

            check_result("逐轴取限（各归各，越界）",
                         emaster_target_guard_check(targets, actual, limits, 2U, &axis, &offset),
                         EMASTER_TARGET_GUARD_TOO_FAR);
            if (axis != 1U)
            {
                fprintf(stderr, "FAIL 逐轴取限越界轴号: expected 1, got %zu\n", axis);
                ++failures;
            }
            check_u64("逐轴取限越界偏移", offset, 250U);
        }
    }

    /* 上限为 0 的轴不判：换算不可用或未配置时，越界也不拦。 */
    {
        const int32_t targets[2] = {999999, 1000};
        const int32_t actual[2] = {0, 1000};
        const uint64_t limits[2] = {0U, 200U};

        check_result("上限为 0 的轴不判",
                     emaster_target_guard_check(targets, actual, limits, 2U, NULL, NULL),
                     EMASTER_TARGET_GUARD_OK);
    }

    /* 轴数为 0：通过。 */
    check_result("零轴", emaster_target_guard_check(NULL, NULL, NULL, 0U, NULL, NULL),
                 EMASTER_TARGET_GUARD_OK);

    /* 负方向越界也要拦，且偏移取绝对值。 */
    {
        const int32_t targets[1] = {-1400};
        const int32_t actual[1] = {-1000};
        const uint64_t limits[1] = {200U};
        uint64_t offset = 0U;

        check_result("负方向越界",
                     emaster_target_guard_check(targets, actual, limits, 1U, NULL, &offset),
                     EMASTER_TARGET_GUARD_TOO_FAR);
        check_u64("负方向偏移", offset, 400U);
    }

    if (failures != 0)
    {
        fprintf(stderr, "target_guard: %d 项不符\n", failures);
        return 1;
    }
    printf("target_guard: 全部通过\n");
    return 0;
}
