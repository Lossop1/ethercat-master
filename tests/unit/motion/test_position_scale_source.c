/*
 * 换算的来源：设备用 608F/6091 回答了，还是设备档声明的。
 *
 * MT 实测（2026-10-14）：驱动器的对象字典里没有 608F/6091/607E，只有手册的示例代码
 * 写着 0x607A 的单位是编码器增量（"Target: 1 turn (for 17-bit)"）、一圈 131072 个计数，
 * 减速比 19.612:1 来自厂商上位机。于是"这台设备的换算从哪来"成了运行期必须回答的问题，
 * 而回答它的那一半逻辑是纯的，在这里钉死：
 *
 *   1. 来源决定"能不能用"：三种取值里只有两种能用，"没有"一律不能用；
 *   2. 设备档声明的换算与设备读回的换算走同一条折算路径 —— 声明值不是次等公民，
 *      只是记录下来的来源不同；
 *   3. 半份换算（减速比的分母为零）一律折不出来。按半份算出来的角度不会报错，
 *      只会按错误的系数动轴；
 *   4. 运动方案对账（expected_position_scale）也认设备档声明的换算，否则这台设备
 *      永远挂不上运动方案。
 *
 * 红了先看 include/emaster/motion/relative_position.h 与 session_setup.c 的兜底：
 * 来源是"这些数从哪来"的记录，不是"值对不对"的判据 —— 后者由各自的折算函数做。
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "emaster/motion/relative_position.h"

static int failures = 0;

static void check_bool(const char *name, bool actual, bool expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %s, got %s\n", name,
                expected ? "true" : "false", actual ? "true" : "false");
        ++failures;
    }
}

static void check_i64(const char *name, int64_t actual, int64_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %lld, got %lld\n", name,
                (long long)expected, (long long)actual);
        ++failures;
    }
}

/* MT 那台：编码器一圈 131072 个计数，减速比 19612:1000 = 19.612:1。 */
static emaster_position_scale_t mt_scale(emaster_fact_source_t source)
{
    emaster_position_scale_t scale;

    scale.source = source;
    scale.encoder_increments = 131072U;
    scale.encoder_motor_revolutions = 1U;
    scale.gear_motor_revolutions = 19612U;
    scale.gear_shaft_revolutions = 1000U;
    return scale;
}

static void check_usable(void)
{
    emaster_position_scale_t none = mt_scale(EMASTER_FACT_SOURCE_NONE);

    check_bool("空指针不可用", emaster_position_scale_is_usable(NULL), false);
    check_bool("来源为空不可用", emaster_position_scale_is_usable(&none), false);
    none = mt_scale(EMASTER_FACT_SOURCE_DEVICE);
    check_bool("设备读回的可用", emaster_position_scale_is_usable(&none), true);
    none = mt_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    check_bool("设备档声明的可用", emaster_position_scale_is_usable(&none), true);
}

static void check_declared_scale_converts(void)
{
    emaster_position_scale_t scale = mt_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    int64_t counts = 0;

    /* 30° = 30000 毫度：30000/360000 × 131072 × 19612/1000 = 214215.34 → 214215。 */
    check_bool("声明的换算折得出 30°",
               emaster_motion_angle_to_counts(30000, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &scale, &counts),
               true);
    check_i64("30° 的计数", counts, INT64_C(214215));
    /* 反向：-30000 毫度。 */
    check_bool("声明的换算折得出 -30°",
               emaster_motion_angle_to_counts(-30000, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &scale, &counts),
               true);
    check_i64("-30° 的计数", counts, INT64_C(-214215));
}

static void check_missing_source_refuses(void)
{
    emaster_position_scale_t scale = mt_scale(EMASTER_FACT_SOURCE_NONE);
    int64_t counts = 0;

    check_bool("没有来源时折不出来",
               emaster_motion_angle_to_counts(30000, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &scale, &counts),
               false);
}

static void check_half_scale_refuses(void)
{
    emaster_position_scale_t scale = mt_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    int64_t counts = 0;

    /* 减速比的分母为零：这是"声明了一半"，不是"减速比为零"。 */
    scale.gear_shaft_revolutions = 0U;
    check_bool("分母为零折不出来",
               emaster_motion_angle_to_counts(30000, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &scale, &counts),
               false);
    check_bool("半份换算是可用的（可用与否只看来源）",
               emaster_position_scale_is_usable(&scale),
               true);
}

static void check_expectation_accepts_declared(void)
{
    emaster_position_scale_t scale = mt_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    emaster_position_scale_t none = mt_scale(EMASTER_FACT_SOURCE_NONE);
    emaster_motion_axis_config_t axis_config = {
        .expected_encoder_increments = 131072U,
        .expected_encoder_motor_revolutions = 1U,
        .expected_gear_motor_revolutions = 19612U,
        .expected_gear_shaft_revolutions = 1000U,
    };

    /* 设备档声明的换算与方案里写的期望值相同就该放行：否则这台设备永远挂不上方案。 */
    check_bool("声明的换算与期望值相符", emaster_position_scale_matches(&axis_config, &scale),
               true);
    /* 没有来源的换算不参与对账（值是零，对不上任何期望值）。 */
    check_bool("没有来源的换算对不上帐", emaster_position_scale_matches(&axis_config, &none),
               false);
}

int main(void)
{
    check_usable();
    check_declared_scale_converts();
    check_missing_source_refuses();
    check_half_scale_refuses();
    check_expectation_accepts_declared();

    if (failures != 0)
    {
        fprintf(stderr, "position_scale_source: %d 项不符\n", failures);
        return 1;
    }
    printf("position_scale_source: 全部通过\n");
    return 0;
}
