/*
 * 滑动窗口计数的回归测试。
 *
 * 现场来源（2026-09-16 台架长跑，报告 runtime/reports/archive/20260916-*）：
 * 25.4 分钟里出现 50 次整帧缺失（WKC=-1），每次都是孤立的、下一拍就恢复
 * （wkc_no_frame_max_consecutive_errors = 1），期间零截止时间超时、跟随误差正常。
 * 但 default 策略的 total_error_threshold = 50 按整轮会话累计，于是第 50 次把这次
 * 健康的会话判死（status_code=16 WKC_MISMATCH）。本文件第一条断言复刻的就是这个
 * 现场：同样的 50 次错误，摊在 25 分钟里不该判死，挤在 10 秒里必须判死。
 *
 * 编译与运行（本机 MinGW 与 Pi 都适用；不依赖 SOEM）：
 *   gcc -std=c11 -O2 -Iinclude -o tmp/test_window \
 *       tests/unit/cyclic/test_window.c src/cyclic/window.c
 *   ./tmp/test_window
 */

#include "emaster/cyclic/window.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* 常量一律用 uint64：60000 * 1000000 会在 int 里溢出，正是这类表达式最容易写错的地方。 */
static const uint64_t MS_NS = UINT64_C(1000000);
/* 与 default 策略一致：60 秒窗口、50 次为限。 */
static const uint64_t POLICY_WINDOW_NS = UINT64_C(60000) * UINT64_C(1000000);
static const int POLICY_THRESHOLD = 50;

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        printf("  [失败] %s\n", what);
    }
}

static void check_u64(uint64_t actual, uint64_t expected, const char *what)
{
    if (actual != expected)
    {
        ++failures;
        printf("  [失败] %s：期望 %" PRIu64 "，实际 %" PRIu64 "\n", what, expected, actual);
    }
}

/*
 * 现场复刻：50 次孤立错误按 30 秒一次摊开，窗口内计数始终应该很小 —— 会话不该判死。
 * 同时给出反例：同样 50 次挤进 10 秒，窗口计数必须到 50，会话必须判死。
 */
static void test_field_scenario(void)
{
    emaster_cyclic_window_t spread;
    emaster_cyclic_window_t burst;
    uint64_t max_spread = 0U;
    uint64_t max_burst = 0U;
    uint64_t now;

    printf("现场复刻：50 次错误摊在 25 分钟 vs 挤在 10 秒\n");
    emaster_cyclic_window_init(&spread, POLICY_WINDOW_NS);
    /* 起点先推进一次，否则第一次 record 只是建立时基，不参与统计。 */
    (void)emaster_cyclic_window_record(&spread, 0U);
    now = 0U;
    for (int index = 1; index <= 50; ++index)
    {
        now = (uint64_t)index * 30U * 1000U * MS_NS;
        uint64_t count = emaster_cyclic_window_record(&spread, now);
        if (count > max_spread)
        {
            max_spread = count;
        }
    }
    check(max_spread < POLICY_THRESHOLD,
          "30 秒一次的错误不该在 60 秒窗口里攒到 50 次（长跑自杀正是这么发生的）");
    printf("  摊开 25 分钟：窗口内峰值 %" PRIu64 " 次（阈值 %d）\n", max_spread, POLICY_THRESHOLD);

    emaster_cyclic_window_init(&burst, POLICY_WINDOW_NS);
    (void)emaster_cyclic_window_record(&burst, 0U);
    for (int index = 1; index <= 50; ++index)
    {
        /* 10 秒内 50 次，远快于 60 秒窗口。 */
        now = (uint64_t)index * 200U * MS_NS;
        uint64_t count = emaster_cyclic_window_record(&burst, now);
        if (count > max_burst)
        {
            max_burst = count;
        }
    }
    check(max_burst >= POLICY_THRESHOLD,
          "10 秒内 50 次必须到阈值 —— 否则窗口把真故障也一起放过了");
    printf("  挤在 10 秒：窗口内峰值 %" PRIu64 " 次（阈值 %d）\n", max_burst, POLICY_THRESHOLD);
}

static void test_window_contents(void)
{
    emaster_cyclic_window_t window;

    printf("窗口内容与滑出\n");
    /* window_ns = 1000ms，桶宽 31.25ms。 */
    emaster_cyclic_window_init(&window, 1000U * MS_NS);
    check_u64(window.bucket_ns, 31250000U, "桶宽应为 window_ns / 桶数");

    check_u64(emaster_cyclic_window_record(&window, 0U), 1U, "第一次记账");
    check_u64(emaster_cyclic_window_record(&window, 10U * MS_NS), 2U, "同一窗口内累加");
    check_u64(emaster_cyclic_window_record(&window, 20U * MS_NS), 3U, "同一窗口内继续累加");
    check_u64(emaster_cyclic_window_count(&window, 30U * MS_NS), 3U, "取值不记账");

    /* 距第一次记账恰好 (桶数-1) 个桶：最早的桶还在窗口里，这是窗口的上边界。 */
    check_u64(emaster_cyclic_window_record(&window, 968750000U), 4U,
              "恰好 (N-1) 个桶时最早的事件仍在窗口内");

    /* 再往前一个桶：t=0 那个桶（3 条）整体滑出，只剩 968.75ms 和本次这两条。 */
    check_u64(emaster_cyclic_window_record(&window, 1000000000U), 2U,
              "滑出的桶必须整桶从合计里减掉");

    /* 整个窗口过期。 */
    check_u64(emaster_cyclic_window_record(&window, 5000000000U), 1U, "整窗过期后只剩本次");
}

static void test_long_gap_clears(void)
{
    emaster_cyclic_window_t window;

    printf("长间隔与边界\n");
    emaster_cyclic_window_init(&window, 1000U * MS_NS);
    (void)emaster_cyclic_window_record(&window, 0U);
    (void)emaster_cyclic_window_record(&window, MS_NS);
    /* 间隔远超窗口：逐步清空与一次性清空必须给出同一结果。 */
    check_u64(emaster_cyclic_window_count(&window, 10U * 1000U * MS_NS), 0U,
              "超过窗口 10 倍后窗口应为空");

    /* window_ns = 0 的退化输入不得除零。 */
    emaster_cyclic_window_init(&window, 0U);
    check_u64(window.bucket_ns, 1U, "window_ns=0 时桶宽退回 1ns");
    check_u64(emaster_cyclic_window_record(&window, 100U), 1U, "退化窗口仍可记账");
    check_u64(emaster_cyclic_window_record(&window, 200U), 1U, "退化窗口按 1ns 桶推进");

    /* 空指针不得崩。 */
    emaster_cyclic_window_init(NULL, 1000U);
    check_u64(emaster_cyclic_window_record(NULL, 0U), 0U, "空指针返回 0");
    check_u64(emaster_cyclic_window_count(NULL, 0U), 0U, "空指针返回 0");
}

static void test_clock_not_advancing(void)
{
    emaster_cyclic_window_t window;

    printf("时钟不回退\n");
    emaster_cyclic_window_init(&window, 1000U * MS_NS);
    check_u64(emaster_cyclic_window_record(&window, 1000U * MS_NS), 1U, "首次记账");
    /* CLOCK_MONOTONIC 不该回退，但真回退了也不能把计数算错。 */
    check_u64(emaster_cyclic_window_record(&window, 500U * MS_NS), 2U,
              "时钟回退时不推进，事件仍计入当前桶");
    check_u64(emaster_cyclic_window_record(&window, 1000U * MS_NS), 3U, "回到原时刻仍不推进");
}

int main(void)
{
    test_field_scenario();
    test_window_contents();
    test_long_gap_clears();
    test_clock_not_advancing();

    if (failures != 0)
    {
        printf("窗口测试失败：%d 项\n", failures);
        return 1;
    }
    printf("窗口测试全部通过\n");
    return 0;
}
