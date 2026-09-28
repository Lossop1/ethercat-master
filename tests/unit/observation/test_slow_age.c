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
 *   3. 603F 的字段级年龄（见下）；
 *   4. 轴号越界、axis_count=0（观测线程一次都没发布过）→ 没有年龄；
 *   5. 时钟异常（now 比 last_read 小）→ 年龄取 0，不返回负数、不返回"没有"；
 *   6. 年龄的输出参数可以为 NULL（只要"有没有"这个答案）；
 *   7. 时间戳要真的穿过 seqlock 发布/读取那条路（发布是按字段拷贝 + axes 整块
 *      memcpy 的，将来有人改成逐字段拷贝而漏掉新字段，这里会红）。
 *
 * 第 3 节问的是**另一个**函数：603F（错误码）那套**字段级**的年龄（P12.17 的补充仪表）。
 * 第 4~7 节里凡带"603F"字样的断言也在问它。它与轴级那套不是重复：
 *   - 温度读到了、错误码这一轮超时，是常事。共用轴级标志会把快照里残留的 0x0000
 *     当成"运行期读到的 0x0000"印出去——正是 P11.8 那个形状换了根轴。
 *   - 所以这里同时钉住："读到 0x0000"算读到了（驱动器不报错就是报 0），
 *     "轴级读到了但错误码这轮没读到"必须仍然答"没有"。
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

/* 603F 的时间戳与轴级时间戳分开给：两者独立保留，正是要钉住的那条性质。 */
static void set_error_code(emaster_observation_slow_state_t *state, uint32_t index,
                           uint16_t code, uint64_t last_read_ns)
{
    state->axes[index].error_code = code;
    state->axes[index].error_code_last_read_ns = last_read_ns;
}

/* 0x607A 同理：值与时间戳独立给，好把"值还在、这一轮没读到"造出来。 */
static void set_target_position(emaster_observation_slow_state_t *state, uint32_t index,
                                int32_t value, uint64_t last_read_ns)
{
    state->axes[index].target_position_readback = value;
    state->axes[index].target_position_readback_last_read_ns = last_read_ns;
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

    /* ── 3. 603F 的字段级年龄 ─────────────────────────────────────────── */
    printf("603F 的字段级年龄\n");
    /* 轴 0：读到过 0x2310。轴 1：读到过 0x0000——"读到 0"也是读到了。 */
    set_error_code(&state, 0U, UINT16_C(0x2310), now - UINT64_C(90000000));
    set_error_code(&state, 1U, UINT16_C(0x0000), now - UINT64_C(30000000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_error_code_age(&state, 0U, now, &age),
          "读到过 603F 的轴应当有错误码年龄");
    check(age == UINT64_C(90000000), "错误码年龄 = now - 读到它的时刻");
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_error_code_age(&state, 1U, now, &age),
          "**读到 0x0000 也算读到了**——驱动器不报错就是报 0，不许当成'没读到'");
    check(age == UINT64_C(30000000), "读到 0x0000 时年龄照给");

    /*
     * 这条是整节的关键：轴级时间戳有值（温度读到了），603F 的时间戳为 0（这一轮的
     * 错误码读超时）。轴级 age 答"有"，字段级 age 必须答"没有"——共用轴级标志的
     * 实现会在这里红，那正是把残留的 0x0000 印成真读数的那条路。
     */
    set_axis(&state, 2U, false, now - UINT64_C(510000000));
    set_error_code(&state, 2U, UINT16_C(0x0000), UINT64_C(0));
    check(emaster_observation_slow_axis_age(&state, 2U, now, NULL),
          "这一轴的温度读到过（轴级时间戳有值）");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_error_code_age(&state, 2U, now, &age),
          "但 603F 从没读到过——字段级必须答'没有'，不许拿轴级时间戳顶替");
    check(age == AGE_SENTINEL, "答'没有'时不得写输出参数");

    /* ── 3b. 0x607A（驱动器自报的目标位置）的字段级年龄 ───────────────── */
    /*
     * 它与 603F 都走字段级，但**失败后果不是一回事**，所以两条都要问：603F 读失败把
     * 故障时刻挪后，是个偏差；0x607A 读失败会把"没读到"下成"驱动器收到的目标一直是
     * 旧值"——排查轴不动时最想下、也最容易下错的那个结论（P12.41）。
     */
    printf("0x607A 的字段级年龄（P12.41）\n");
    set_target_position(&state, 0U, INT32_C(207180), now - UINT64_C(60000000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_target_position_age(&state, 0U, now, &age),
          "读到过 0x607A 的轴应当有读回年龄");
    check(age == UINT64_C(60000000), "读回年龄 = now - 读到它的时刻");
    /*
     * 读到 0 也算读到了。"值是旧值"这件事要由**值本身**说明，不由标志说明——
     * 标志答的永远是"这条 SDO 这一轮回来了没有"。
     */
    set_target_position(&state, 1U, INT32_C(0), now - UINT64_C(7000000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_target_position_age(&state, 1U, now, &age),
          "读到 0 也算读到了——判据是读到没读到，不是值好不好看");
    check(age == UINT64_C(7000000), "读到 0 时年龄照给");

    /* 与 603F 那条同形：轴级时间戳有值，这一条没有。 */
    set_axis(&state, 3U, false, now - UINT64_C(520000000));
    set_target_position(&state, 3U, INT32_C(1031), UINT64_C(0));
    check(emaster_observation_slow_axis_age(&state, 3U, now, NULL),
          "轴 3 的温度读到过（轴级时间戳有值）");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_target_position_age(&state, 3U, now, &age),
          "但 0x607A 从没读到过——字段级必须答'没有'，不许拿轴级时间戳顶替");
    check(age == AGE_SENTINEL, "答'没有'时不得写输出参数");

    /* ── 4. 越界与没发布过 ─────────────────────────────────────────────── */
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
    check(!emaster_observation_slow_axis_error_code_age(&empty, 0U, now, &age),
          "603F：axis_count=0 时同样没有年龄");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_error_code_age(&state, 5U, now, &age) &&
              !emaster_observation_slow_axis_error_code_age(
                  &state, EMASTER_OBSERVATION_MAX_AXES, now, &age),
          "603F：轴号越界（含 >= 容量上限）应当没有年龄，且不得越界读数组");
    check(age == AGE_SENTINEL, "603F 越界时不得写输出参数");
    check(!emaster_observation_slow_axis_target_position_age(&empty, 0U, now, &age),
          "0x607A：axis_count=0 时同样没有年龄");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_target_position_age(&state, 5U, now, &age) &&
              !emaster_observation_slow_axis_target_position_age(
                  &state, EMASTER_OBSERVATION_MAX_AXES, now, &age),
          "0x607A：轴号越界（含 >= 容量上限）应当没有年龄，且不得越界读数组");
    check(age == AGE_SENTINEL, "0x607A 越界时不得写输出参数");

    /* ── 5. 时钟异常不给负数 ───────────────────────────────────────────── */
    printf("时钟异常\n");
    set_axis(&state, 1U, true, now + UINT64_C(5000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_age(&state, 1U, now, &age),
          "now 比 last_read 小时，读数仍然是读到的");
    check(age == UINT64_C(0), "时钟异常时年龄取 0，不给负数");
    set_error_code(&state, 1U, UINT16_C(0x2310), now + UINT64_C(5000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_error_code_age(&state, 1U, now, &age) &&
              age == UINT64_C(0),
          "603F：时钟异常时同样取 0，不给负数");
    set_target_position(&state, 1U, INT32_C(207180), now + UINT64_C(5000));
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_target_position_age(&state, 1U, now, &age) &&
              age == UINT64_C(0),
          "0x607A：时钟异常时同样取 0，不给负数");

    /* ── 6. 只要答案不要年龄 ───────────────────────────────────────────── */
    printf("输出参数可以为空\n");
    check(emaster_observation_slow_axis_age(&state, 0U, now, NULL),
          "不关心年龄时也应当能问'有没有'");
    check(emaster_observation_slow_axis_error_code_age(&state, 0U, now, NULL),
          "603F：不关心年龄时也应当能问'有没有'");
    check(emaster_observation_slow_axis_target_position_age(&state, 0U, now, NULL),
          "0x607A：不关心年龄时也应当能问'有没有'");

    /* ── 7. 时间戳要真的穿过发布/读取 ─────────────────────────────────── */
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
    check(readback.axes[0].error_code_last_read_ns ==
              state.axes[0].error_code_last_read_ns,
          "603F 的时间戳也必须原样穿过发布/读取（它是新字段，最容易被漏掉）");
    check(readback.axes[2].error_code_last_read_ns == UINT64_C(0),
          "从没读到过的 603F 时间戳穿过发布之后必须仍然是 0（不是被别的轴顶替）");
    check(readback.axes[0].target_position_readback_last_read_ns ==
              state.axes[0].target_position_readback_last_read_ns,
          "0x607A 的时间戳也必须原样穿过发布/读取");
    check(readback.axes[3].target_position_readback_last_read_ns == UINT64_C(0),
          "0x607A 从没读到过的时间戳穿过发布之后必须仍然是 0");
    /*
     * 值也要搬。标志与值是两列，只搬一列在"读到过"的轴上分不出来，在没读到的轴上
     * 才露馅：这时值必须仍是快照里那个旧值，而不是被清成 0——清成 0 就把"上一轮
     * 驱动器报的还是那个数"改写成"驱动器报 0"了。
     */
    check(readback.axes[3].target_position_readback == INT32_C(1031),
          "0x607A 的值也要穿过发布，没读到的轴不得被清成 0");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_target_position_age(&readback, 3U, now, &age) &&
              age == AGE_SENTINEL,
          "从发布回来的快照里，0x607A 没读到的轴仍然答'没有'");
    age = AGE_SENTINEL;
    check(emaster_observation_slow_axis_age(&readback, 0U, now, &age) &&
              age == UINT64_C(182000000),
          "从发布回来的快照取年龄，结果应当与发布前一致");
    age = AGE_SENTINEL;
    check(!emaster_observation_slow_axis_error_code_age(&readback, 2U, now, &age) &&
              age == AGE_SENTINEL,
          "从发布回来的快照里，603F 没读到的轴仍然答'没有'");

    if (failures != 0U)
    {
        printf("慢速遥测年龄：%u 项不符\n", failures);
        return 1;
    }
    printf("慢速遥测年龄：全部通过\n");
    return 0;
}
