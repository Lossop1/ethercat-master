/*
 * 慢速遥测"读到了没有、多久以前读到的"的回归测试。
 *
 * 现场来源（2026-09-18 第二轮四臂台架，P11.4 / P11.8）：停机信号落在一轮 SDO 轮询
 * 中途，正在读的那一轮被截断，报告里轴表末尾连续几根的温度/电压印成 0——而 0 与
 * "这一轴压根没读到"在报告里长得一模一样。四臂的对照是这样的（stop_site=1 表示
 * 停在邮箱往返里）：
 *
 *   臂        stop_site  stop_axis  报告里为 0 的轴
 *   solo          0          4       无
 *   obs           0          4       无
 *   bridge        1          0       2、3、4、5
 *   drive         1          4       无
 *
 * 机理：写样本只在校验成功时改字段，值从上一轮起就留在快照里；被截断的是**最后一轮**
 * 的 valid（它回答"这一轮读没读到"）。所以判据不能拿 valid 用——那正是把"上一轮读到的
 * 42.0 °C"印成 0 的那一行。本测试钉住的就是这条分界：
 *
 *   0. 从没读到过（快照归零后的原样）→ 没有年龄，且 **不写** 输出参数；
 *   1. 读到过 → 年龄 = now - last_read_ns；
 *   2. **被截断的那一轮**（valid=false、last_read_ns 是上一轮留下的）→ 仍然有年龄。
 *      这一条是判别力的关键：实现退回去按 valid 判，它必红；
 *   3. 轴号越界、axis_count=0（观测线程一次都没发布过）→ 没有年龄；
 *   4. 时钟异常（now 比 last_read 小）→ 年龄取 0，不返回负数、不返回"没有"；
 *   5. 年龄的输出参数可以为 NULL（只要"有没有"这个答案）；
 *   6. 时间戳要真的穿过 seqlock 发布/读取那条路（发布是按字段拷贝 + axes 整块
 *      memcpy 的，将来有人改成逐字段拷贝而漏掉新字段，这里会红）。
 *
 * 编译与运行（本机 MinGW 与 Pi 都适用；不依赖 SOEM、不访问总线）：
 *   gcc -std=c11 -O2 -Iinclude -o tmp/test_slow_age \
 *       tests/unit/observation/test_slow_age.c src/bus/observation/slow.c
 *   ./tmp/test_slow_age
 */
#include "emaster/observation/slow.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 哨兵：用来证明"没有年龄"的那条路径真的没写输出参数。 */
#define AGE_SENTINEL UINT64_C(0xDEADBEEF)

static unsigned int failures;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        printf("  [不符] %s\n", what);
    }
}

/* 造一轴：valid 与 last_read_ns 分开给，好把"本轮没读到但以前读到过"造出来。 */
static void set_axis(emaster_observation_slow_state_t *state, uint32_t index, bool valid,
                     uint64_t last_read_ns)
{
    state->axes[index].valid = valid;
    state->axes[index].last_read_ns = last_read_ns;
    state->axes[index].mosfet_temperature = 420 + (int32_t)index;
}

int main(void)
{
    emaster_observation_slow_state_t state;
    uint64_t age = AGE_SENTINEL;
    const uint64_t now = UINT64_C(9000000000);

    /* ── 0. 归零的快照：从没读到过 ─────────────────────────────────────── */
    printf("从没读到过\n");
    memset(&state, 0, sizeof(state));
    state.axis_count = 5U;
    for (uint32_t index = 0U; index < 5U; ++index)
    {
        set_axis(&state, index, false, UINT64_C(0));
    }
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_age(&state, 0U, now, &age),
          "从没读到过的轴应当没有年龄");
    check(age == AGE_SENTINEL, "没有年龄时不得写输出参数（0 是合法的年龄）");

    /* ── 1. 读到过 ─────────────────────────────────────────────────────── */
    printf("读到过：年龄 = now - 读到的时刻\n");
    set_axis(&state, 0U, true, now - UINT64_C(182000000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_age(&state, 0U, now, &age),
          "读到的轴应当有年龄");
    check(age == UINT64_C(182000000), "年龄应当是 182 ms（这就是四臂里实测的量级）");

    /* ── 2. 被截断的那一轮：valid=false，但值还在 ─────────────────────── */
    printf("停机截断的那一轮（P11.8 的判别点）\n");
    const uint64_t last_round_ns = now - UINT64_C(410000000); /* 上一轮读到的时刻 */
    for (uint32_t index = 2U; index < 5U; ++index)
    {
        /* 轴 2~4：本轮被截断（valid=false），值来自 410 ms 前那一次读。 */
        set_axis(&state, index, false, last_round_ns);
    }
    for (uint32_t index = 2U; index < 5U; ++index)
    {
        age = AGE_SENTINEL;
        check(emaster_observation_slow_axis_age(&state, index, now, &age),
              "本轮被截断的轴，只要以前读到过就必须有年龄——按 valid 判会在这里红");
        check(age == UINT64_C(410000000), "年龄来自最后一次读到的那一轮，不是最后一轮");
    }

    /* ── 3. 越界与没发布过 ─────────────────────────────────────────────── */
    printf("越界与没发布过\n");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_age(&state, 5U, now, &age),
          "轴号 >= axis_count（快照里没有这一轴）应当没有年龄");
    check(age == AGE_SENTINEL, "越界时不得写输出参数");
    check(!emaster_observation_slow_axis_age(&state, EMASTER_OBSERVATION_MAX_AXES, now, &age),
          "轴号 >= 快照容量上限应当没有年龄（不得越界读数组）");
    emaster_observation_slow_state_t empty = {0};
    check(!emaster_observation_slow_axis_age(&empty, 0U, now, &age),
          "axis_count=0（观测线程一次都没发布过）应当没有年龄");

    /* ── 4. 时钟异常不给负数 ───────────────────────────────────────────── */
    printf("时钟异常\n");
    set_axis(&state, 1U, true, now + UINT64_C(5000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_age(&state, 1U, now, &age),
          "now 比 last_read 小时，读数仍然是读到的");
    check(age == UINT64_C(0), "时钟异常时年龄取 0，不给负数");

    /* ── 5. 只要答案不要年龄 ───────────────────────────────────────────── */
    printf("输出参数可以为空\n");
    check(emaster_observation_slow_axis_age(&state, 0U, now, NULL),
          "不关心年龄时也应当能问'有没有'");

    /* ── 6. 时间戳要真的穿过发布/读取 ─────────────────────────────────── */
    printf("时间戳穿过 seqlock\n");
    static emaster_observation_slow_t snapshot; /* 静态：这块近 1 KiB，别压在栈上 */
    emaster_observation_slow_state_t published;
    emaster_observation_slow_state_t readback;

    emaster_observation_slow_init(&snapshot);
    published = state;
    published.cycle = 12345U;
    published.read_count = 42U;
    emaster_observation_slow_publish(&snapshot, &published);
    check(emaster_observation_slow_read(&snapshot, &readback), "刚发布的快照应当读得到");
    check(readback.axes[0].last_read_ns == state.axes[0].last_read_ns,
          "发布/读取之后时间戳必须原样还在（逐字段拷贝漏掉新字段会在这里红）");
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_age(&readback, 0U, now, &age) &&
              age == UINT64_C(182000000),
          "从发布回来的快照取年龄，结果应当与发布前一致");

    if (failures != 0U)
    {
        printf("慢速遥测年龄：%u 项不符\n", failures);
        return 1;
    }
    printf("慢速遥测年龄：全部通过\n");
    return 0;
}
