/*
 * 换算的来源：设备用 608F/6091 回答了，还是设备档声明的。
 *
 * 有些驱动器的对象字典里没有 608F/6091/607E，换算只能由设备档声明；手册也只写 0x607A
 * 的单位是编码器增量、一圈多少个计数。声明值未必等于硬件真值（厂商上位机给的比值与实机
 * 对不上是出现过的），所以下面夹具里那个比值是任取的。于是"这台设备的换算从哪来"成了
 * 运行期必须回答的问题，
 * 而回答它的那一半逻辑是纯的，在这里钉死：
 *
 *   1. 来源决定"能不能用"：三种取值里只有两种能用，"没有"一律不能用；
 *   2. 设备档声明的换算与设备读回的换算走同一条折算路径 —— 声明值不是次等公民，
 *      只是记录下来的来源不同；
 *   3. 半份换算（减速比的分母为零）一律折不出来。按半份算出来的角度不会报错，
 *      只会按错误的系数动轴；
 *   4. 运动方案对账（expected_position_scale）也认设备档声明的换算，否则这台设备
 *      永远挂不上运动方案；
 *   5. 设备档声明必须三项齐全才算声明过，缺一项就整块不认、一个字段都不写 ——
 *      半份换算折算出来的计数看着像真的，只会让轴按错的系数动。
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

/* 折算算术的夹具：编码器一圈 131072 个计数，减速比 19612:1000 = 19.612:1。
 * 比值任取 —— 这几条测的是折算怎么算，不是这台设备的换算（见文件头）。 */
static emaster_position_scale_t ratio_scale(emaster_fact_source_t source)
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
    emaster_position_scale_t none = ratio_scale(EMASTER_FACT_SOURCE_NONE);

    check_bool("空指针不可用", emaster_position_scale_is_usable(NULL), false);
    check_bool("来源为空不可用", emaster_position_scale_is_usable(&none), false);
    none = ratio_scale(EMASTER_FACT_SOURCE_DEVICE);
    check_bool("设备读回的可用", emaster_position_scale_is_usable(&none), true);
    none = ratio_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    check_bool("设备档声明的可用", emaster_position_scale_is_usable(&none), true);
}

static void check_declared_scale_converts(void)
{
    emaster_position_scale_t scale = ratio_scale(EMASTER_FACT_SOURCE_CONFIGURED);
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
    emaster_position_scale_t scale = ratio_scale(EMASTER_FACT_SOURCE_NONE);
    int64_t counts = 0;

    check_bool("没有来源时折不出来",
               emaster_motion_angle_to_counts(30000, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &scale, &counts),
               false);
}

static void check_half_scale_refuses(void)
{
    emaster_position_scale_t scale = ratio_scale(EMASTER_FACT_SOURCE_CONFIGURED);
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

/* 设备档声明的三项齐全才叫声明过；缺一项就不是换算。 */
static void check_from_declaration(void)
{
    emaster_position_scale_t scale;
    bool ok;

    /* 三项齐全：照抄，来源记为"设备档声明"，电机侧转数固定为 1。 */
    scale.source = EMASTER_FACT_SOURCE_NONE;
    ok = emaster_position_scale_from_declaration(131072U, 19612U, 1000U, &scale);
    check_bool("三项齐全时接受", ok, true);
    check_bool("来源记为设备档声明", scale.source == EMASTER_FACT_SOURCE_CONFIGURED, true);
    check_i64("编码器每转计数照抄", (int64_t)scale.encoder_increments, 131072);
    check_i64("电机侧转数固定为 1", (int64_t)scale.encoder_motor_revolutions, 1);
    check_i64("减速比电机侧照抄", (int64_t)scale.gear_motor_revolutions, 19612);
    check_i64("减速比负载侧照抄", (int64_t)scale.gear_shaft_revolutions, 1000);

    /* 缺任何一项都算"一半的换算"：返回 false，而且**一个字段都不许写**。
     * 写了一半比不写更坏：来源一旦非零，后面那道门就放行，轴按错的系数动。 */
    {
        static const char *names[] = {"缺编码器计数", "缺减速比电机侧", "缺减速比负载侧"};
        uint32_t missing[][3] = {
            {0U, 19612U, 1000U},
            {131072U, 0U, 1000U},
            {131072U, 19612U, 0U},
        };

        for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i)
        {
            emaster_position_scale_t untouched;

            untouched.source = EMASTER_FACT_SOURCE_NONE;
            untouched.encoder_increments = 7U;
            untouched.encoder_motor_revolutions = 7U;
            untouched.gear_motor_revolutions = 7U;
            untouched.gear_shaft_revolutions = 7U;
            scale = untouched;
            ok = emaster_position_scale_from_declaration(missing[i][0], missing[i][1],
                                                         missing[i][2], &scale);
            check_bool(names[i], ok, false);
            check_bool("拒绝时来源没被改动",
                       scale.source == EMASTER_FACT_SOURCE_NONE, true);
            check_bool("拒绝时一个字段都没写",
                       scale.encoder_increments == untouched.encoder_increments &&
                           scale.encoder_motor_revolutions ==
                               untouched.encoder_motor_revolutions &&
                           scale.gear_motor_revolutions ==
                               untouched.gear_motor_revolutions &&
                           scale.gear_shaft_revolutions ==
                               untouched.gear_shaft_revolutions,
                       true);
            check_bool("拒绝的换算不可用", emaster_position_scale_is_usable(&scale), false);
        }
    }

    check_bool("空输出指针不崩且拒绝",
               emaster_position_scale_from_declaration(131072U, 19612U, 1000U, NULL), false);
}

static void check_expectation_accepts_declared(void)
{
    emaster_position_scale_t scale = ratio_scale(EMASTER_FACT_SOURCE_CONFIGURED);
    emaster_position_scale_t none = ratio_scale(EMASTER_FACT_SOURCE_NONE);
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
    check_from_declaration();
    check_expectation_accepts_declared();

    if (failures != 0)
    {
        fprintf(stderr, "position_scale_source: %d 项不符\n", failures);
        return 1;
    }
    printf("position_scale_source: 全部通过\n");
    return 0;
}
