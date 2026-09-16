/*
 * 观测通道离线自测。不需要 EtherCAT 硬件，也不需要 SOEM——这是把 emaster::observation
 * 拆成独立库的主要理由：整条通道除"周期线程发布那一行调用点"以外都能在这里验证。
 *
 * 这里验的不是"跑得通"，是四条性质：
 *
 * 1. **不撕裂**。读者拿到的 OK 帧必须是一整帧同源数据，不能是两拍拼出来的混合物。
 * 2. **覆盖语义**。请求已被覆盖的序号必须拿到 STALE，而不是半截帧。
 * 3. **写者无感**。读者的数量、快慢、是否卡死，都不改变写者的行为。这是
 *    "观测不扰动控制"的操作性判据。
 * 4. **协议可检出畸形**。线格式的版本、长度、轴数必须自洽，错一个字节就得整条拒绝，
 *    而不是按错位偏移解出一堆"看起来合理"的数字。
 *
 * 关键手法：每一条"零撕裂"的断言旁边都配一个**故意的错误读者**做对照。没有对照，
 * "零撕裂"可能只是因为测试根本没制造出竞争——那种通过毫无价值。所以：
 *
 *   - 环形缓冲：无校验探针直接读 head % CAP 那一格（写者手上的那格），必须能观察到
 *     撕裂。它证明危险确实存在，因此 oldest_readable() 抬高一格不是多余的。
 *   - 慢速 seqlock：跳过序号校验的探针必须能观察到不一致快照。它证明这个交错里撕裂
 *     确实可达，带校验读者的"零撕裂"才有意义。
 *
 * 编译与运行（本机与 Pi 都适用）：
 *   cmake --build build --target observation-selftest
 *   ./build/tools/observation_selftest/observation-selftest
 */
#define _POSIX_C_SOURCE 200809L

#include "emaster/observation/ring.h"
#include "emaster/observation/slow.h"
#include "emaster/observation/wire.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 测试里用的轴数与 WKC。真实部署是 4 轴、期望 WKC 12，取同量级更有代表性。 */
#define TEST_AXES 4U
#define TEST_WKC 12

/* 并发测试的帧数。取到百万量级是为了让"写者正在写某格槽"这个窄窗口被撞上足够多次。 */
#define TEST_CONCURRENT_FRAMES UINT64_C(1000000)

/* 慢速 seqlock 测试的发布次数。 */
#define TEST_SLOW_PUBLISHES UINT64_C(20000)

/* 写者独立性测试里的读者线程数。 */
#define TEST_WRITER_READERS 4U

/*
 * 读者每转这么多圈让出一次 CPU。满速自旋的读者会把写者（本进程主线程，普通优先级）
 * 从 CPU 上挤下去，测出来的就成了操作系统的调度延迟而不是写者的行为——在核数少于
 * "读者数 + 1" 的 CI runner 上这是必然发生的。让出一次既保留了 head 那一行的争用，
 * 又不会饿死写者。
 */
#define READER_YIELD_INTERVAL 64U

/*
 * 慢速写者每发布一次空转的圈数。没有它，写者会在读者两次校验之间发布成千上万次，
 * 交错窗口被压得极窄，对照探针就观察不到不一致快照——测试会因为"没制造出竞争"而
 * 失去意义（自测里把这条写成了显式断言）。
 */
#define SLOW_WRITER_SPIN 500U

static int g_checks;
static int g_failures;

#define CHECK(condition, ...)                                                              \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                  \
        {                                                                                  \
            ++g_failures;                                                                  \
            fprintf(stderr, "  失败 %s:%d：", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
        }                                                                                  \
    } while (0)

static void section(const char *name)
{
    printf("== %s\n", name);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * 造一帧内容，全部字段都是 index 的函数。任何两拍之间的字段值都不同，所以
 * "两拍混在一起"一定能被 frame_matches() 认出来——这是撕裂检测的基础。
 *
 * publish_index 刻意不在这里填：它由环形缓冲的发布函数负责，是槽归属的唯一凭据。
 */
static void fill_frame(emaster_observation_frame_t *frame, uint64_t index)
{
    uint16_t axis_index;

    emaster_observation_frame_clear(frame, TEST_AXES);
    frame->cycle = index * UINT64_C(3) + UINT64_C(7);
    frame->monotonic_ns = index * UINT64_C(1000000);
    frame->deadline_ns = frame->monotonic_ns + UINT64_C(250000);
    frame->frame_interval_ns = UINT64_C(1000000);
    frame->wkc = (int32_t)TEST_WKC;
    frame->flags = (uint32_t)(index & UINT64_C(0x7));

    for (axis_index = 0U; axis_index < TEST_AXES; ++axis_index)
    {
        emaster_observation_axis_t *axis = &frame->axes[axis_index];

        axis->actual_position = (int32_t)(index * UINT64_C(31) + axis_index);
        axis->target_position = (int32_t)(index * UINT64_C(31) + axis_index + UINT64_C(1000));
        axis->actual_velocity = -(int32_t)(index * UINT64_C(7) + axis_index);
        axis->actual_torque = (int32_t)(index * UINT64_C(13) + axis_index * UINT64_C(3));
        axis->status_word = (uint16_t)(UINT16_C(0x0637) + axis_index);
        axis->control_word = (uint16_t)(UINT16_C(0x000F) + axis_index);
        axis->flags = (uint32_t)(index & UINT64_C(0x3)) + axis_index;
    }
}

/* 逐字段比对，不用 memcmp：填充字节是否被拷贝是编译器的自由，不该混进判定。 */
static bool axis_matches(const emaster_observation_axis_t *actual,
                         const emaster_observation_axis_t *expected)
{
    return actual->actual_position == expected->actual_position &&
           actual->target_position == expected->target_position &&
           actual->actual_velocity == expected->actual_velocity &&
           actual->actual_torque == expected->actual_torque &&
           actual->status_word == expected->status_word &&
           actual->control_word == expected->control_word && actual->flags == expected->flags;
}

/*
 * frame 的载荷是否恰好是 index 这一拍的内容，**不看 publish_index**。
 * 单独拆出来是为了分辨"整圈错位"和"同一槽的两次发布混在一起"。
 */
static bool frame_payload_matches(const emaster_observation_frame_t *frame, uint64_t index)
{
    emaster_observation_frame_t expected;
    uint16_t axis_index;

    fill_frame(&expected, index);
    if (frame->cycle != expected.cycle || frame->monotonic_ns != expected.monotonic_ns ||
        frame->deadline_ns != expected.deadline_ns ||
        frame->frame_interval_ns != expected.frame_interval_ns || frame->wkc != expected.wkc ||
        frame->flags != expected.flags || frame->axis_count != expected.axis_count)
    {
        return false;
    }
    for (axis_index = 0U; axis_index < expected.axis_count; ++axis_index)
    {
        if (!axis_matches(&frame->axes[axis_index], &expected.axes[axis_index]))
        {
            return false;
        }
    }
    return true;
}

/* frame 是否恰好是 index 这一拍的内容（含 publish_index）。 */
static bool frame_matches(const emaster_observation_frame_t *frame, uint64_t index)
{
    return frame->publish_index == index && frame_payload_matches(frame, index);
}

/* ---------------------------------------------------------------- 边界与窗口 */

static void test_ring_windows(void)
{
    static emaster_observation_ring_t ring;
    emaster_observation_frame_t frame;
    emaster_observation_frame_t out;
    uint64_t oldest;
    uint64_t newest;
    uint64_t index;
    const uint64_t capacity = (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY;

    section("环形缓冲：边界与可读窗口");

    emaster_observation_ring_init(&ring);
    CHECK(emaster_observation_ring_head(&ring) == 0U, "初始 head 应为 0");
    CHECK(!emaster_observation_ring_window(&ring, &oldest, &newest), "空缓冲不应给出窗口");
    CHECK(emaster_observation_ring_read(&ring, 0U, &out) == EMASTER_OBSERVATION_READ_EMPTY,
          "空缓冲应返回 EMPTY");

    /* 发布到 head = CAP - 1：此时还没有任何一帧被覆盖，窗口下界是 0。 */
    for (index = 0U; index + 1U < capacity; ++index)
    {
        fill_frame(&frame, index);
        emaster_observation_ring_publish(&ring, &frame);
    }
    CHECK(emaster_observation_ring_head(&ring) == capacity - 1U, "head 应为 CAP-1");
    CHECK(emaster_observation_ring_window(&ring, &oldest, &newest) && oldest == 0U &&
              newest == capacity - 1U,
          "head=CAP-1 时窗口应为 [0, CAP-1)");
    CHECK(emaster_observation_ring_read(&ring, 0U, &out) == EMASTER_OBSERVATION_READ_OK &&
              frame_matches(&out, 0U),
          "head=CAP-1 时第 0 帧应可读且内容自洽");
    CHECK(emaster_observation_ring_read(&ring, capacity - 1U, &out) ==
              EMASTER_OBSERVATION_READ_STALE,
          "请求尚未发布的序号应返回 STALE");
    CHECK(emaster_observation_ring_read(&ring, capacity, &out) ==
              EMASTER_OBSERVATION_READ_STALE,
          "请求远未来的序号应返回 STALE");

    /*
     * 发布到 head = CAP。第 0 帧所在的槽与写者手上的那一格是同一个（CAP % CAP == 0），
     * 必须被排除。这条用例抓出过 oldest_readable() 写成 `>` 而不是 `>=` 的差一错误。
     */
    fill_frame(&frame, capacity - 1U);
    emaster_observation_ring_publish(&ring, &frame);
    CHECK(emaster_observation_ring_head(&ring) == capacity, "head 应为 CAP");
    CHECK(emaster_observation_ring_window(&ring, &oldest, &newest) && oldest == 1U,
          "head=CAP 时窗口下界应为 1（最老一帧不可读）");
    CHECK(emaster_observation_ring_read(&ring, 0U, &out) == EMASTER_OBSERVATION_READ_STALE,
          "head=CAP 时第 0 帧（写者手上的那格）必须 STALE");
    CHECK(emaster_observation_ring_read(&ring, 1U, &out) == EMASTER_OBSERVATION_READ_OK &&
              frame_matches(&out, 1U),
          "head=CAP 时第 1 帧应可读");

    /* 再推一格：窗口下界随之前移。 */
    fill_frame(&frame, capacity);
    emaster_observation_ring_publish(&ring, &frame);
    CHECK(emaster_observation_ring_window(&ring, &oldest, &newest) && oldest == 2U &&
              newest == capacity + 1U,
          "head=CAP+1 时窗口应为 [2, CAP+1)");
    CHECK(emaster_observation_ring_read(&ring, 1U, &out) == EMASTER_OBSERVATION_READ_STALE,
          "head=CAP+1 时第 1 帧应已被覆盖");
    CHECK(emaster_observation_ring_read(&ring, 2U, &out) == EMASTER_OBSERVATION_READ_OK &&
              frame_matches(&out, 2U),
          "head=CAP+1 时第 2 帧应可读");

    /* 一路推到整整两圈，确认覆盖只按环形推进，不出现越界或误判可读。 */
    for (index = capacity + 1U; index < capacity * 2U; ++index)
    {
        fill_frame(&frame, index);
        emaster_observation_ring_publish(&ring, &frame);
    }
    CHECK(emaster_observation_ring_head(&ring) == capacity * 2U, "head 应为 2*CAP");
    CHECK(emaster_observation_ring_window(&ring, &oldest, &newest) && oldest == capacity + 1U &&
              newest == capacity * 2U,
          "head=2*CAP 时窗口应为 [CAP+1, 2*CAP)");
    CHECK(emaster_observation_ring_read(&ring, capacity, &out) ==
              EMASTER_OBSERVATION_READ_STALE,
          "head=2*CAP 时第 CAP 帧应已被覆盖");
    CHECK(emaster_observation_ring_read(&ring, capacity + 1U, &out) ==
              EMASTER_OBSERVATION_READ_OK &&
              frame_matches(&out, capacity + 1U),
          "head=2*CAP 时第 CAP+1 帧应可读");
    CHECK(emaster_observation_ring_read(&ring, capacity * 2U - 1U, &out) ==
              EMASTER_OBSERVATION_READ_OK &&
              frame_matches(&out, capacity * 2U - 1U),
          "head=2*CAP 时最新一帧应可读");

    /* 轴数超上限时按上限截断，而不是写出数组外。 */
    {
        emaster_observation_frame_t oversize;

        emaster_observation_frame_clear(&oversize, (uint16_t)(EMASTER_OBSERVATION_MAX_AXES + 5U));
        emaster_observation_ring_publish(&ring, &oversize);
        CHECK(emaster_observation_ring_read(&ring, capacity * 2U, &out) ==
                      EMASTER_OBSERVATION_READ_OK &&
                  out.axis_count == (uint16_t)EMASTER_OBSERVATION_MAX_AXES,
              "超上限的轴数应被截断到 MAX_AXES");
    }
}

/* ---------------------------------------------------------------- 并发读者 */

typedef struct
{
    const emaster_observation_ring_t *ring;
    _Atomic bool stop;
    _Atomic uint64_t ok;
    _Atomic uint64_t stale;
    _Atomic uint64_t empty;
    _Atomic uint64_t unstable;
    /* 读过 OK 但内容对不上：撕裂。唯一真正不能出现的计数。 */
    _Atomic uint64_t torn;
    /* 读到的 head 比上一次小：head 单调性被破坏。 */
    _Atomic uint64_t head_regression;
    /* read_or_latest 在该降级的时候没有降级。 */
    _Atomic uint64_t degraded_mismatch;
    /*
     * 第一次撕裂的现场。用来分辨两种完全不同的成因，而它们需要完全不同的修法：
     *
     * - **读者侧内存序缺口**：拷贝与复查之间的载荷读被重排到复查之后，于是复查报
     *   "head 没变"，载荷却读到了写者后来写进去的下一圈内容。特征是帧里的
     *   publish_index 仍等于请求的序号（槽的归属标签还没被改写），而载荷对不上。
     * - **协议本身有洞**：可读窗口算错，读者读到了写者手上的那一格。特征是
     *   publish_index 与请求序号不符，或载荷与请求序号相差整一圈。
     *
     * x86 是强内存序（TSO），载荷读不会被重排到后面的读之后，因此本机永远看不到
     * 第一种；aarch64 是弱内存序，才会露出来。
     */
    _Atomic bool torn_recorded;
    _Atomic uint64_t torn_index;
    _Atomic uint64_t torn_head;
    _Atomic uint64_t torn_publish;
    /* 首次撕裂的错位形态，见 torn_kind_* 常量。 */
    _Atomic unsigned torn_kind;
    /* 撕裂帧的载荷与 index±CAP 相符的次数：相符即"整圈错位"，不符即"混了"。 */
    _Atomic uint64_t torn_next_lap;
    _Atomic uint64_t torn_prev_lap;
} reader_stats_t;

/* 首次撕裂的错位形态。见 note_tear。 */
#define TORN_KIND_MIXED 0U    /* 载荷既不等于 index 也不等于 index±CAP：混了两拍 */
#define TORN_KIND_NEXT_LAP 1U /* 载荷整份等于 index+CAP：整圈错位到下一圈 */
#define TORN_KIND_PREV_LAP 2U /* 载荷整份等于 index-CAP：整圈错位到上一圈 */

static void stats_reset(reader_stats_t *stats)
{
    atomic_store_explicit(&stats->stop, false, memory_order_relaxed);
    atomic_store_explicit(&stats->ok, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->stale, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->empty, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->unstable, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->head_regression, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->degraded_mismatch, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_recorded, false, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_index, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_head, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_publish, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_kind, TORN_KIND_MIXED, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_next_lap, 0U, memory_order_relaxed);
    atomic_store_explicit(&stats->torn_prev_lap, 0U, memory_order_relaxed);
}

static void stats_count(_Atomic uint64_t *counter)
{
    atomic_fetch_add_explicit(counter, 1U, memory_order_relaxed);
}

/*
 * 记一次撕裂，并留下第一次的现场。第一次才是能定因果的那次：后面的都被后续撕裂
 * 覆盖了。这与报告里 first_mismatch_* 只记首次是同一个手法。
 */
static void note_tear(reader_stats_t *stats,
                      const emaster_observation_frame_t *frame,
                      uint64_t index)
{
    const uint64_t capacity = (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY;
    uint64_t head = emaster_observation_ring_head(stats->ring);
    unsigned kind = TORN_KIND_MIXED;

    stats_count(&stats->torn);
    if (frame_payload_matches(frame, index + capacity))
    {
        stats_count(&stats->torn_next_lap);
        kind = TORN_KIND_NEXT_LAP;
    }
    else if (index >= capacity && frame_payload_matches(frame, index - capacity))
    {
        stats_count(&stats->torn_prev_lap);
        kind = TORN_KIND_PREV_LAP;
    }
    if (!atomic_exchange_explicit(&stats->torn_recorded, true, memory_order_relaxed))
    {
        atomic_store_explicit(&stats->torn_kind, kind, memory_order_relaxed);
        atomic_store_explicit(&stats->torn_index, index, memory_order_relaxed);
        atomic_store_explicit(&stats->torn_head, head, memory_order_relaxed);
        atomic_store_explicit(&stats->torn_publish, frame->publish_index,
                              memory_order_relaxed);
    }
}

/*
 * 把首次撕裂的现场打出来。读者能读到 OK，说明它自己那两道判据都过了：复查的 head
 * 与首读相同、且槽里的 publish_index 等于请求序号。所以现场里 publish_index **一定**
 * 等于请求序号，能分辨成因的只剩载荷错位到哪一圈。
 *
 * 而协议本身可以证明"整圈错位到上一圈"不是写者的错：写者只在 head == index 时写槽
 * index % CAP，读者只在 index ≥ oldest(head) = head - CAP + 1 时才取该槽，两者交集
 * 为空。若现场真的出现上一圈命中，这条证明就被证伪了，那时要查的是窗口算术，不是
 * 内存序。
 */
static void report_tear_scene(const reader_stats_t *stats)
{
    uint64_t index = atomic_load(&stats->torn_index);
    uint64_t head = atomic_load(&stats->torn_head);
    uint64_t publish = atomic_load(&stats->torn_publish);
    unsigned kind = atomic_load(&stats->torn_kind);

    printf("    首次撕裂现场：请求 index=%" PRIu64 " head=%" PRIu64
           " 帧内 publish_index=%" PRIu64 " 下一圈命中=%" PRIu64 " 上一圈命中=%" PRIu64 "\n",
           index, head, publish, atomic_load(&stats->torn_next_lap),
           atomic_load(&stats->torn_prev_lap));
    if (kind == TORN_KIND_NEXT_LAP)
    {
        printf("    错位形态：载荷整份来自 index+CAP（槽标签仍是本圈）。\n");
    }
    else if (kind == TORN_KIND_PREV_LAP)
    {
        printf("    错位形态：载荷整份来自 index-CAP —— 与可读窗口的证明矛盾，"
               "要先查窗口算术。\n");
    }
    else
    {
        printf("    错位形态：载荷既不等于本圈也不等于邻圈 —— 同一次拷贝里混了两拍。\n");
    }
    if (publish != index)
    {
        printf("    注意：publish_index 与请求序号不符，撕裂不在 read() 的判据之内"
               "（对照探针直接读槽，属于预期）。\n");
    }
}

/* 每 READER_YIELD_INTERVAL 次读让出一次 CPU。见该宏处的说明。 */
static void pause_periodically(unsigned *iterations_since_yield)
{
    ++(*iterations_since_yield);
    if (*iterations_since_yield >= READER_YIELD_INTERVAL)
    {
        *iterations_since_yield = 0U;
        (void)sched_yield();
    }
}

/*
 * 读者 A：永远读最新一帧。这是与写者竞争最激烈的位置——写者刚写完 head 就轮到我们。
 * 只要 oldest_readable() 的推导成立，这里就不该出现撕裂。
 */
static void *reader_newest(void *arg)
{
    reader_stats_t *stats = arg;
    uint64_t last_head = 0U;
    unsigned since_yield = 0U;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        uint64_t head;
        emaster_observation_frame_t frame;

        pause_periodically(&since_yield);
        head = emaster_observation_ring_head(stats->ring);
        if (head < last_head)
        {
            stats_count(&stats->head_regression);
        }
        last_head = head;
        if (head == 0U)
        {
            stats_count(&stats->empty);
            continue;
        }
        switch (emaster_observation_ring_read(stats->ring, head - 1U, &frame))
        {
        case EMASTER_OBSERVATION_READ_OK:
            if (!frame_matches(&frame, head - 1U))
            {
                note_tear(stats, &frame, head - 1U);
            }
            stats_count(&stats->ok);
            break;
        case EMASTER_OBSERVATION_READ_STALE:
            stats_count(&stats->stale);
            break;
        case EMASTER_OBSERVATION_READ_EMPTY:
            stats_count(&stats->empty);
            break;
        case EMASTER_OBSERVATION_READ_UNSTABLE:
        default:
            stats_count(&stats->unstable);
            break;
        }
    }
    return NULL;
}

/*
 * 读者 B：永远请求 head - CAP，也就是被 oldest_readable() 排除掉的那一格。
 * 它必须是 STALE——无论写者有没有推进。这条断言把"排除最老一帧"从注释变成了测试。
 */
static void *reader_oldest(void *arg)
{
    reader_stats_t *stats = arg;
    const uint64_t capacity = (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY;
    unsigned since_yield = 0U;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        uint64_t head;
        emaster_observation_frame_t frame;

        pause_periodically(&since_yield);
        head = emaster_observation_ring_head(stats->ring);
        if (head <= capacity)
        {
            continue;
        }
        switch (emaster_observation_ring_read(stats->ring, head - capacity, &frame))
        {
        case EMASTER_OBSERVATION_READ_STALE:
            stats_count(&stats->stale);
            break;
        case EMASTER_OBSERVATION_READ_OK:
            /* 读到了！要么是排除逻辑没生效，要么是它返回了内容对不上的帧。 */
            if (!frame_matches(&frame, head - capacity))
            {
                note_tear(stats, &frame, head - capacity);
            }
            stats_count(&stats->ok);
            break;
        default:
            stats_count(&stats->unstable);
            break;
        }
    }
    return NULL;
}

/* 读者 C：窗口里随机挑一帧。只有 STALE 与 OK 是合法结局。 */
static void *reader_window(void *arg)
{
    reader_stats_t *stats = arg;
    uint64_t cursor = 0U;
    unsigned since_yield = 0U;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        uint64_t oldest;
        uint64_t newest;

        pause_periodically(&since_yield);
        ++cursor;
        if (!emaster_observation_ring_window(stats->ring, &oldest, &newest))
        {
            stats_count(&stats->empty);
            continue;
        }
        if (newest <= oldest)
        {
            stats_count(&stats->stale);
            continue;
        }
        {
            uint64_t index = oldest + (cursor % (newest - oldest));
            emaster_observation_frame_t frame;

            switch (emaster_observation_ring_read(stats->ring, index, &frame))
            {
            case EMASTER_OBSERVATION_READ_OK:
                if (!frame_matches(&frame, index))
                {
                    note_tear(stats, &frame, index);
                }
                stats_count(&stats->ok);
                break;
            case EMASTER_OBSERVATION_READ_STALE:
                stats_count(&stats->stale);
                break;
            default:
                stats_count(&stats->unstable);
                break;
            }
        }
    }
    return NULL;
}

/*
 * 读者 D：故意请求一个早就被覆盖的序号，验证降级语义。
 *
 * 缓冲区还没绕满一圈时序号 0 仍然可读，此时"未降级"才是正确结局；绕满一圈之后
 * （head > CAP）序号 0 永远在最老一格之前，降级才是唯一正确结局。
 */
static void *reader_degraded(void *arg)
{
    reader_stats_t *stats = arg;
    unsigned since_yield = 0U;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        emaster_observation_frame_t frame;
        bool degraded = false;

        pause_periodically(&since_yield);
        if (emaster_observation_ring_head(stats->ring) <=
            (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY)
        {
            continue;
        }
        switch (emaster_observation_ring_read_or_latest(stats->ring, 0U, &frame, &degraded))
        {
        case EMASTER_OBSERVATION_READ_OK:
            if (!degraded)
            {
                stats_count(&stats->degraded_mismatch);
            }
            if (!frame_matches(&frame, frame.publish_index))
            {
                note_tear(stats, &frame, frame.publish_index);
            }
            stats_count(&stats->ok);
            break;
        case EMASTER_OBSERVATION_READ_EMPTY:
            stats_count(&stats->empty);
            break;
        default:
            stats_count(&stats->unstable);
            break;
        }
    }
    return NULL;
}

/*
 * 对照探针：直接读 head % CAP 那一格——**写者手上的那一格**，跳过一切校验。
 * 它必须在同一个交错里观察到撕裂。否则说明这个测试根本没制造出竞争，别处的
 * "零撕裂"就是空的。
 *
 * 这一段故意制造数据竞争（读者与写者同时访问同一个槽），是测试手段，不是疏漏。
 */
static void *reader_naive_probe(void *arg)
{
    reader_stats_t *stats = arg;
    const uint64_t capacity = (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY;
    unsigned since_yield = 0U;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        uint64_t head;
        emaster_observation_frame_t raw;

        pause_periodically(&since_yield);
        head = emaster_observation_ring_head(stats->ring);
        if (head == 0U)
        {
            continue;
        }
        memcpy(&raw, &stats->ring->slots[(size_t)(head % capacity)], sizeof(raw));
        if (frame_matches(&raw, head))
        {
            stats_count(&stats->ok);
        }
        else if (head >= capacity && frame_matches(&raw, head - capacity))
        {
            /* 读到了上一圈那一帧：写者正占着这格槽，还没写完。 */
            stats_count(&stats->stale);
        }
        else
        {
            /* 两圈都不是：两拍混在一起了。 */
            note_tear(stats, &raw, head);
        }
    }
    return NULL;
}

static void test_ring_concurrent(const char *title, void *(*reader)(void *), bool expect_torn)
{
    static emaster_observation_ring_t ring;
    reader_stats_t stats;
    pthread_t thread;
    emaster_observation_frame_t frame;
    uint64_t index;

    emaster_observation_ring_init(&ring);
    stats.ring = &ring;
    stats_reset(&stats);

    CHECK(pthread_create(&thread, NULL, reader, &stats) == 0, "读者线程创建失败");
    for (index = 0U; index < TEST_CONCURRENT_FRAMES; ++index)
    {
        fill_frame(&frame, index);
        emaster_observation_ring_publish(&ring, &frame);
    }
    atomic_store_explicit(&stats.stop, true, memory_order_relaxed);
    CHECK(pthread_join(thread, NULL) == 0, "读者线程 join 失败");

    printf("  %-22s 读 OK=%-10" PRIu64 " STALE=%-10" PRIu64 " EMPTY=%-8" PRIu64
           " UNSTABLE=%-10" PRIu64 " 撕裂=%" PRIu64 "\n",
           title, atomic_load(&stats.ok), atomic_load(&stats.stale),
           atomic_load(&stats.empty), atomic_load(&stats.unstable), atomic_load(&stats.torn));

    if (atomic_load(&stats.torn) > 0U)
    {
        report_tear_scene(&stats);
    }

    if (expect_torn)
    {
        /*
         * 对照探针的通过条件是"**必须**看到问题"。看不到问题说明这个交错太温和，
         * 别的断言因此不可采信——所以要报失败，而不是报通过。
         */
        CHECK(atomic_load(&stats.torn) > 0U,
              "对照探针没有观察到撕裂，说明本次交错没有制造出竞争，其余断言不可采信");
    }
    else
    {
        CHECK(atomic_load(&stats.torn) == 0U, "出现了撕裂帧");
        CHECK(atomic_load(&stats.head_regression) == 0U, "head 出现回退");
        CHECK(atomic_load(&stats.degraded_mismatch) == 0U, "read_or_latest 的降级判定不对");
    }
}

static void test_ring_concurrency(void)
{
    section("环形缓冲：并发与对抗读者（每项 100 万帧）");
    test_ring_concurrent("读者-最新", reader_newest, false);
    test_ring_concurrent("读者-窗口随机", reader_window, false);
    test_ring_concurrent("读者-降级取最新", reader_degraded, false);
    test_ring_concurrent("读者-被排除的最老格", reader_oldest, false);
    /* 对照：跳过校验的探针必须自己撞上撕裂 */
    test_ring_concurrent("对照-无校验探针", reader_naive_probe, true);
}

/* ---------------------------------------------------------------- 写者独立性 */

/* 单次发布的耗时上界：周期的 1%。 */
#define PUBLISH_BUDGET_NS UINT64_C(10000)

/*
 * 读者在场时允许的相对抬升：p50 最多是独跑的这么多倍，另加一点固定余量。
 *
 * 8 倍不是"实测到的代价"，实测是 4.2–5.8 倍（WSL/2 vCPU）与 6.0–7.8 倍（Orange Pi，
 * 12 核；p50 31 → 183–241 ns），来源是观测环 head 所在缓存行被反复作废——真实且预期。
 *
 * 两台机器的**绝对值**差了 5 倍，比值却是小机器更小、大机器更大：比值本身不是稳定的
 * 机器无关量。所以判据写成"倍数 + 固定项"，由固定项在比值抖动时兜底（Orange Pi 上
 * 8×31 = 248 ns 已接近实测上限，全靠这 500 ns 撑开余量）。
 *
 * 它是**烟雾报警器**而不是性能规格：写者真开始等读者是几十倍到毫秒级，不是 5 倍。
 */
#define PUBLISH_CONTENTION_FACTOR 8U
#define PUBLISH_CONTENTION_SLACK_NS UINT64_C(500)

/* 每 BATCH 帧取一次时钟：逐帧测会把 clock_gettime 本身算进被测对象。 */
#define PUBLISH_BATCH 1000U
#define PUBLISH_BATCH_MAX 4096U

typedef struct
{
    uint64_t total_ns;
    uint64_t batch_ns[PUBLISH_BATCH_MAX];
    size_t batch_count;
} publish_span_t;

/*
 * 计时口径刻意不用"最慢一批"。写者是本进程的主线程，读者是若干个自旋线程：在没有
 * 实时优先级的机器上，主线程被 OS 换出一次就会让某一批变成毫秒级——那是调度器的事，
 * 不是"写者在等读者"。拿最大值当判据，CI runner 上必然假红。
 *
 * 改成取批耗时分位：真正失效（写者阻塞）会让**大量**批次一起变慢，分位数一定会抬起
 * 来；偶发的一次换出只污染最高的一两批。最大值仍然打印出来供人看。
 */
static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    if (a < b)
    {
        return -1;
    }
    if (a > b)
    {
        return 1;
    }
    return 0;
}

/* 把批耗时排序后取分位，折算成单帧。span 的批数组会被排序破坏（最大值不受影响）。 */
static uint64_t span_percentile_per_frame(publish_span_t *span, unsigned percent)
{
    size_t index;

    if (span->batch_count == 0U)
    {
        return 0U;
    }
    qsort(span->batch_ns, span->batch_count, sizeof(uint64_t), compare_u64);
    index = (span->batch_count * (size_t)percent) / 100U;
    if (index >= span->batch_count)
    {
        index = span->batch_count - 1U;
    }
    return span->batch_ns[index] / PUBLISH_BATCH;
}

static uint64_t span_max_per_frame(const publish_span_t *span)
{
    uint64_t worst = 0U;
    size_t batch;

    for (batch = 0U; batch < span->batch_count; ++batch)
    {
        if (span->batch_ns[batch] > worst)
        {
            worst = span->batch_ns[batch];
        }
    }
    return worst / PUBLISH_BATCH;
}

static uint64_t span_total_per_frame(const publish_span_t *span, uint64_t frames)
{
    if (frames == 0U)
    {
        return 0U;
    }
    return span->total_ns / frames;
}

static void publish_span_ns(emaster_observation_ring_t *ring, uint64_t frames,
                            publish_span_t *out)
{
    emaster_observation_frame_t frame;
    uint64_t index;
    uint64_t batch_start = monotonic_ns();

    memset(out, 0, sizeof(*out));
    for (index = 0U; index < frames; ++index)
    {
        fill_frame(&frame, index);
        emaster_observation_ring_publish(ring, &frame);
        if ((index + 1U) % PUBLISH_BATCH == 0U)
        {
            uint64_t elapsed = monotonic_ns() - batch_start;

            out->total_ns += elapsed;
            if (out->batch_count < (size_t)PUBLISH_BATCH_MAX)
            {
                out->batch_ns[out->batch_count] = elapsed;
                ++out->batch_count;
            }
            batch_start = monotonic_ns();
        }
    }
}

static void test_writer_independence(void)
{
    static emaster_observation_ring_t ring;
    static publish_span_t alone;
    static publish_span_t contended;
    reader_stats_t stats;
    pthread_t threads[TEST_WRITER_READERS];
    uint64_t p50_alone;
    uint64_t p50_contended;
    uint64_t p99_alone;
    uint64_t p99_contended;
    uint64_t mean_alone;
    uint64_t mean_contended;
    unsigned thread_index;

    section("写者独立性：四个自旋读者不得拖慢发布");

    emaster_observation_ring_init(&ring);
    publish_span_ns(&ring, TEST_CONCURRENT_FRAMES, &alone);

    emaster_observation_ring_init(&ring);
    stats.ring = &ring;
    stats_reset(&stats);
    for (thread_index = 0U; thread_index < TEST_WRITER_READERS; ++thread_index)
    {
        CHECK(pthread_create(&threads[thread_index], NULL,
                             (thread_index % 2U == 0U) ? reader_newest : reader_window,
                             &stats) == 0,
              "读者线程创建失败");
    }
    publish_span_ns(&ring, TEST_CONCURRENT_FRAMES, &contended);
    atomic_store_explicit(&stats.stop, true, memory_order_relaxed);
    for (thread_index = 0U; thread_index < TEST_WRITER_READERS; ++thread_index)
    {
        CHECK(pthread_join(threads[thread_index], NULL) == 0, "读者线程 join 失败");
    }

    p99_alone = span_percentile_per_frame(&alone, 99U);
    p50_alone = span_percentile_per_frame(&alone, 50U);
    mean_alone = span_total_per_frame(&alone, TEST_CONCURRENT_FRAMES);
    p99_contended = span_percentile_per_frame(&contended, 99U);
    p50_contended = span_percentile_per_frame(&contended, 50U);
    mean_contended = span_total_per_frame(&contended, TEST_CONCURRENT_FRAMES);

    printf("  独跑         p50 %" PRIu64 " ns/帧，p99 批 %" PRIu64 "，最慢批 %" PRIu64
           "（均值 %" PRIu64 "）\n",
           p50_alone, p99_alone, span_max_per_frame(&alone), mean_alone);
    printf("  %u 读者自旋 p50 %" PRIu64 " ns/帧，p99 批 %" PRIu64 "，最慢批 %" PRIu64
           "（均值 %" PRIu64 "）\n",
           (unsigned)TEST_WRITER_READERS, p50_contended, p99_contended,
           span_max_per_frame(&contended), mean_contended);

    /*
     * 判据分两层，各自对应一种真实的失效模式：
     *
     * 1. **绝对上界**：批耗时分位折算到单帧不得超过周期的 1%（10 µs）。抓的是
     *    "写者被挡住"——真出现等读者的情况，会是几十微秒到毫秒级的**持续**抬升，
     *    分位数一定会越界。
     * 2. **相对上界**：读者在场时 p50 相对独跑不得超出 PUBLISH_CONTENTION_FACTOR 倍。
     *    抓的是缓存行争用失控。
     *
     * 若干个满速自旋读者是最恶劣的争用形态，真实部署是 50 Hz–1 kHz 的按需拉取，远达
     * 不到这个强度。刻意用最恶劣的形态，判据才有意义。
     *
     * 这两条都**不是**"写者耗时与读者无关"——那句话是错的，缓存行争用确实存在。
     * 能成立的是"写者不等待读者、代码路径不因读者而变"，这里量的是它的后果：常数倍
     * 的缓存行往返，而不是与读者行为相关的停顿。
     *
     * **相对判据必须用 p50，不能用均值。** 均值在这里有结构性偏差：它有偏地把调度器
     * 换出算作争用代价，而且偏差方向与待测量同向——争用越重，写者这一臂的墙钟时间越
     * 长，被 OS 换出的窗口就越多，均值被抬得越高。实测（WSL/2 vCPU）同一份代码均值在
     * 508–704 ns/帧 之间抖，p50 只在 373–551 之间，20 次里 9 次是均值那一项假红。
     * 均值与最慢批仍然打印出来供人看——它们是诊断信息，不是判据。
     *
     * 上界 10 µs 那一项同理：在 2 vCPU 的 WSL 上 p99 批实测到 5.4 ms（1 写者 + 4 自旋
     * 读者 5 个线程抢 2 个核），余量只剩 1.9 倍；同一份代码在 Orange Pi 上 p99 折算
     * 190 ns/帧、与 p50 几乎重合。也就是说这一项在小机器上量的是核数，不是代码。
     * 它若翻红，先看 p50、head、撕裂三处是否**同时**动了：真出现写者停顿，三者会一起
     * 变；只有 p99 孤单地高，那是调度。
     */
    CHECK(p99_contended <= PUBLISH_BUDGET_NS,
          "读者在场时 p99 批耗时折算 %" PRIu64 " ns/帧，越过周期 1%% 的上界 %" PRIu64 " ns",
          p99_contended, PUBLISH_BUDGET_NS);
    CHECK(p50_contended <= p50_alone * PUBLISH_CONTENTION_FACTOR + PUBLISH_CONTENTION_SLACK_NS,
          "读者在场时 p50 %" PRIu64 " ns/帧，超过独跑 %" PRIu64 " ns/帧 的 %u 倍上界",
          p50_contended, p50_alone, (unsigned)PUBLISH_CONTENTION_FACTOR);
    CHECK(emaster_observation_ring_head(&ring) == TEST_CONCURRENT_FRAMES,
          "写者发布的帧数不对：head=%" PRIu64, emaster_observation_ring_head(&ring));
    if (atomic_load(&stats.torn) > 0U)
    {
        report_tear_scene(&stats);
    }
    CHECK(atomic_load(&stats.torn) == 0U, "读者在场时出现了撕裂帧");
}

/* ---------------------------------------------------------------- 慢速 seqlock */

typedef struct
{
    emaster_observation_slow_t *snapshot;
    _Atomic bool stop;
    _Atomic uint64_t ok;
    _Atomic uint64_t failed;
    _Atomic uint64_t torn;
    /*
     * 读到"发布过一次之前的全零状态"的次数，单列出来不并进 torn。
     *
     * 读者线程可能先于写者的首次发布跑起来，此时序号是 0、载荷全零，而它是一份
     * 自洽的（写者没在写）状态，seqlock 检查当然放行。它不是撕裂——是"还没有数据"，
     * 与环形缓冲的 EMPTY 同类。分开计数是为了让这条判定有据可依：若它正好等于
     * 曾经记在 torn 上的数量，就说明那些"撕裂"全是它，seqlock 本身没事。
     */
    _Atomic uint64_t unpublished;
    _Atomic uint64_t unsafe_ok;
    _Atomic uint64_t unsafe_torn;
} slow_stats_t;

/*
 * 状态里每个字段都是轮次的函数。任何两轮混在一起都会被 slow_matches() 认出来。
 * valid_mask 由 publish 计算，这里填的 axes[].valid 必须与之一致。
 */
static void fill_slow(emaster_observation_slow_state_t *state, uint64_t round)
{
    uint32_t axis_index;

    memset(state, 0, sizeof(*state));
    state->cycle = round * UINT64_C(17) + UINT64_C(5);
    state->monotonic_ns = round * UINT64_C(50000000);
    state->axis_count = TEST_AXES;
    state->read_count = (uint32_t)round;
    state->valid_mask = (UINT64_C(1) << TEST_AXES) - UINT64_C(1);
    for (axis_index = 0U; axis_index < TEST_AXES; ++axis_index)
    {
        emaster_observation_slow_axis_t *axis = &state->axes[axis_index];

        axis->valid = true;
        axis->actual_current = (int32_t)(round * UINT64_C(11) + axis_index);
        axis->dc_link_voltage = (int32_t)(UINT64_C(48000) + round);
        axis->mosfet_temperature = (int32_t)(UINT64_C(3000) + round + axis_index);
        axis->motor_temperature = (int32_t)(UINT64_C(2900) + round + axis_index);
        axis->motor_speed = (int32_t)(round * UINT64_C(3) + axis_index);
        axis->speed_command = (int32_t)(round * UINT64_C(3) + axis_index + UINT64_C(1));
        axis->error_code = (uint16_t)(UINT16_C(0x0000) + axis_index);
    }
}

static bool slow_matches(const emaster_observation_slow_state_t *state)
{
    emaster_observation_slow_state_t expected;
    uint32_t axis_index;

    fill_slow(&expected, (uint64_t)state->read_count);
    if (state->cycle != expected.cycle || state->monotonic_ns != expected.monotonic_ns ||
        state->axis_count != expected.axis_count || state->valid_mask != expected.valid_mask)
    {
        return false;
    }
    for (axis_index = 0U; axis_index < expected.axis_count; ++axis_index)
    {
        const emaster_observation_slow_axis_t *actual = &state->axes[axis_index];
        const emaster_observation_slow_axis_t *want = &expected.axes[axis_index];

        if (actual->valid != want->valid || actual->actual_current != want->actual_current ||
            actual->dc_link_voltage != want->dc_link_voltage ||
            actual->mosfet_temperature != want->mosfet_temperature ||
            actual->motor_temperature != want->motor_temperature ||
            actual->motor_speed != want->motor_speed ||
            actual->speed_command != want->speed_command ||
            actual->error_code != want->error_code)
        {
            return false;
        }
    }
    return true;
}

/* 阻止优化掉的空转：累加结果是 volatile 的，编译器不能把循环删掉。 */
static void spin_iterations(uint32_t iterations)
{
    volatile uint32_t sink = 0U;
    uint32_t step;

    for (step = 0U; step < iterations; ++step)
    {
        sink = sink + step;
    }
}

/*
 * 写者起跑前先等一会儿，好让带校验读者**一定**先撞上"还没发布过"的空状态。
 *
 * 这一段是判定依据本身，不是凑数。不带它，读者有没有跑到那个状态取决于两个线程的
 * 起跑偏差：跑到了就多出几十次"载荷对不上"，跑不到就一次没有——而这两种情况在
 * 通过的报告里长得一模一样。断言 slow_stats_t::unpublished 必须被行使过，就是靠它。
 */
static void slow_writer_head_start(void)
{
    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = 20000000L; /* 20 ms：读者在这段时间里会转成千上万圈 */
    (void)nanosleep(&delay, NULL);
}

static void *slow_writer(void *arg)
{
    slow_stats_t *stats = arg;
    emaster_observation_slow_state_t state;
    uint64_t round;

    slow_writer_head_start();
    for (round = 1U; round <= TEST_SLOW_PUBLISHES; ++round)
    {
        fill_slow(&state, round);
        emaster_observation_slow_publish(stats->snapshot, &state);
        spin_iterations(SLOW_WRITER_SPIN);
    }
    return NULL;
}

/* 带校验的读者：唯一合法的结局是"读到一份自洽快照"或"明确没读到"。 */
static void *slow_reader(void *arg)
{
    slow_stats_t *stats = arg;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        emaster_observation_slow_state_t state;

        if (!emaster_observation_slow_read(stats->snapshot, &state))
        {
            atomic_fetch_add_explicit(&stats->failed, 1U, memory_order_relaxed);
            continue;
        }
        /* 发布前的空状态与"载荷对不上"分开记，但两笔都计。见 slow_stats_t 的说明。 */
        if (state.read_count == 0U && state.axis_count == 0U)
        {
            atomic_fetch_add_explicit(&stats->unpublished, 1U, memory_order_relaxed);
        }
        else
        {
            atomic_fetch_add_explicit(&stats->ok, 1U, memory_order_relaxed);
        }
        if (!slow_matches(&state))
        {
            atomic_fetch_add_explicit(&stats->torn, 1U, memory_order_relaxed);
        }
    }
    return NULL;
}

/*
 * 对照读者：跳过序号校验直接拷贝载荷。它必须能观察到不一致快照——否则这个交错里
 * 撕裂根本不可达，带校验读者的"零撕裂"就是在测空气。
 *
 * 同样故意制造数据竞争，属于测试手段。
 */
static void *slow_unsafe_probe(void *arg)
{
    slow_stats_t *stats = arg;

    while (!atomic_load_explicit(&stats->stop, memory_order_relaxed))
    {
        emaster_observation_slow_state_t state;

        memcpy(&state, &stats->snapshot->state, sizeof(state));
        if (slow_matches(&state))
        {
            atomic_fetch_add_explicit(&stats->unsafe_ok, 1U, memory_order_relaxed);
        }
        else
        {
            atomic_fetch_add_explicit(&stats->unsafe_torn, 1U, memory_order_relaxed);
        }
    }
    return NULL;
}

static void test_slow_seqlock(void)
{
    static emaster_observation_slow_t snapshot;
    slow_stats_t stats;
    pthread_t writer;
    pthread_t checked;
    pthread_t unsafe;
    uint64_t checked_ok;
    uint64_t checked_torn;
    uint64_t unsafe_torn;

    section("慢速 seqlock：一致快照与对照探针");

    emaster_observation_slow_init(&snapshot);
    memset(&stats, 0, sizeof(stats));
    stats.snapshot = &snapshot;

    CHECK(pthread_create(&writer, NULL, slow_writer, &stats) == 0, "写者线程创建失败");
    CHECK(pthread_create(&checked, NULL, slow_reader, &stats) == 0, "读者线程创建失败");
    CHECK(pthread_create(&unsafe, NULL, slow_unsafe_probe, &stats) == 0, "对照线程创建失败");
    CHECK(pthread_join(writer, NULL) == 0, "写者线程 join 失败");
    atomic_store_explicit(&stats.stop, true, memory_order_relaxed);
    CHECK(pthread_join(checked, NULL) == 0, "读者线程 join 失败");
    CHECK(pthread_join(unsafe, NULL) == 0, "对照线程 join 失败");

    checked_ok = atomic_load(&stats.ok);
    checked_torn = atomic_load(&stats.torn);
    unsafe_torn = atomic_load(&stats.unsafe_torn);
    printf("  带校验读者：成功 %" PRIu64 " 次，撕裂 %" PRIu64 " 次；重试上限未命中 %" PRIu64
           " 次；发布前的空状态 %" PRIu64 " 次\n",
           checked_ok, checked_torn, atomic_load(&stats.failed),
           atomic_load(&stats.unpublished));
    printf("  对照读者（跳过校验）：一致 %" PRIu64 " 次，不一致 %" PRIu64 " 次\n",
           atomic_load(&stats.unsafe_ok), unsafe_torn);

    CHECK(checked_ok >= 1000U, "带校验读者成功次数太少（%" PRIu64 "），断言没有意义",
          checked_ok);
    /*
     * 先确认这条判定被行使过：读者必须在写者首次发布之前跑过一段，才会读到那个
     * 全零的空状态。没有这一条，下面的等式可能一整轮都是 0 == 0，等于没测。
     */
    CHECK(atomic_load(&stats.unpublished) > 0U,
          "读者没跑到过发布前的空状态，torn == unpublished 这条断言没有被行使");
    /*
     * 带校验读者的**每一处**"载荷对不上"都必须正好是那个发布前的空状态。它是自洽的
     * （写者确实没在写），只是"还没有数据"，与环形缓冲的 EMPTY 同类——不是撕裂。
     * 等式而不是不等式：只要出现一次真正的混合快照，右边就不再相等。
     */
    CHECK(atomic_load(&stats.unpublished) == checked_torn,
          "带校验读者读到不一致快照：撕裂 %" PRIu64 " 次，其中发布前空状态 %" PRIu64 " 次",
          checked_torn, atomic_load(&stats.unpublished));
    CHECK(unsafe_torn > 0U, "对照读者没有观察到不一致快照，本次交错不可采信");
}

/* ---------------------------------------------------------------- 线格式 */

static void test_wire_roundtrip(void)
{
    emaster_observation_frame_t frame;
    emaster_observation_frame_t decoded;
    emaster_observation_wire_header_t header;
    uint8_t buffer[EMASTER_OBSERVATION_WIRE_HEADER_BYTES +
                   (EMASTER_OBSERVATION_MAX_AXES * EMASTER_OBSERVATION_WIRE_AXIS_BYTES)];
    size_t written = 0U;
    uint16_t axis_count;

    section("线格式：往返与长度");

    /* 覆盖 0 轴到上限的全部轴数，外加两个极端值：int32 最小、uint64 最大。 */
    for (axis_count = 0U; axis_count <= (uint16_t)EMASTER_OBSERVATION_MAX_AXES; ++axis_count)
    {
        fill_frame(&frame, UINT64_C(123456789));
        frame.axis_count = axis_count;
        frame.publish_index = UINT64_MAX;
        frame.deadline_ns = UINT64_C(0);
        frame.wkc = INT32_MIN;
        if (axis_count > 0U)
        {
            frame.axes[0].actual_position = INT32_MIN;
            frame.axes[0].target_position = INT32_MAX;
            frame.axes[0].actual_velocity = -1;
            frame.axes[0].actual_torque = 0;
            frame.axes[0].flags = UINT32_MAX;
            frame.axes[0].status_word = UINT16_MAX;
            frame.axes[0].control_word = UINT16_MAX;
        }

        CHECK(emaster_observation_wire_frame_bytes(axis_count) ==
                  (size_t)EMASTER_OBSERVATION_WIRE_HEADER_BYTES +
                      ((size_t)axis_count * (size_t)EMASTER_OBSERVATION_WIRE_AXIS_BYTES),
              "轴数 %u 的报文长度不对", (unsigned)axis_count);
        CHECK(emaster_observation_wire_encode_frame(&frame, buffer, sizeof(buffer), &written),
              "轴数 %u 编码失败", (unsigned)axis_count);
        CHECK(written == emaster_observation_wire_frame_bytes(axis_count),
              "轴数 %u 编码长度不对", (unsigned)axis_count);
        CHECK(emaster_observation_wire_decode_frame(buffer, written, &decoded, &header),
              "轴数 %u 解码失败", (unsigned)axis_count);
        CHECK(header.axis_count == axis_count, "轴数往返不一致");
        CHECK(decoded.publish_index == frame.publish_index && decoded.cycle == frame.cycle &&
                  decoded.monotonic_ns == frame.monotonic_ns &&
                  decoded.deadline_ns == frame.deadline_ns &&
                  decoded.frame_interval_ns == frame.frame_interval_ns &&
                  decoded.wkc == frame.wkc && decoded.flags == frame.flags,
              "轴数 %u 的帧级字段往返不一致", (unsigned)axis_count);
        if (axis_count > 0U)
        {
            CHECK(axis_matches(&decoded.axes[0], &frame.axes[0]), "轴 0 字段往返不一致");
            CHECK(!axis_matches(&decoded.axes[0], &frame.axes[1]), "轴比对函数不敏感");
        }
    }

    /* 缓冲区不够时必须明确失败，不能截断着写。 */
    fill_frame(&frame, UINT64_C(1));
    CHECK(!emaster_observation_wire_encode_frame(&frame, buffer,
                                                 emaster_observation_wire_frame_bytes(1U),
                                                 &written),
          "容量不足时应编码失败");

    /* 小端是显式逐字节写的，与主机字节序无关。这里钉死前几个字节。 */
    fill_frame(&frame, UINT64_C(1));
    CHECK(emaster_observation_wire_encode_frame(&frame, buffer, sizeof(buffer), &written),
          "编码失败");
    CHECK(buffer[0] == (uint8_t)'E' && buffer[1] == (uint8_t)'O', "魔数不对");
    CHECK(buffer[2] == (uint8_t)EMASTER_OBSERVATION_WIRE_VERSION, "版本字节不对");
    CHECK(buffer[3] == (uint8_t)EMASTER_OBSERVATION_WIRE_KIND_FRAME, "类型字节不对");
    CHECK(buffer[4] == (uint8_t)(written & 0xFFU) &&
              buffer[5] == (uint8_t)((written >> 8U) & 0xFFU),
          "frame_bytes 不是小端");
}

static void test_wire_malformed(void)
{
    emaster_observation_frame_t frame;
    emaster_observation_frame_t decoded;
    emaster_observation_wire_header_t header;
    uint8_t buffer[EMASTER_OBSERVATION_WIRE_HEADER_BYTES +
                   (EMASTER_OBSERVATION_MAX_AXES * EMASTER_OBSERVATION_WIRE_AXIS_BYTES)];
    uint8_t mutated[sizeof(buffer)];
    size_t written = 0U;

    section("线格式：畸形输入必须整体拒绝");

    fill_frame(&frame, UINT64_C(42));
    CHECK(emaster_observation_wire_encode_frame(&frame, buffer, sizeof(buffer), &written),
          "编码失败");

    /* 魔数错 */
    memcpy(mutated, buffer, written);
    mutated[0] = (uint8_t)'X';
    CHECK(!emaster_observation_wire_decode_frame(mutated, written, &decoded, &header),
          "魔数错误应被拒绝");

    /* 版本错：这是最危险的一种——按旧偏移硬解会得到错位但"看起来合理"的数字 */
    memcpy(mutated, buffer, written);
    mutated[2] = (uint8_t)(EMASTER_OBSERVATION_WIRE_VERSION + 1U);
    CHECK(!emaster_observation_wire_decode_frame(mutated, written, &decoded, &header),
          "版本不匹配应被拒绝");

    /* 类型错 */
    memcpy(mutated, buffer, written);
    mutated[3] = (uint8_t)0x7F;
    CHECK(!emaster_observation_wire_decode_frame(mutated, written, &decoded, &header),
          "类型错误应被拒绝");

    /* 轴数超上限 */
    memcpy(mutated, buffer, written);
    mutated[6] = (uint8_t)(EMASTER_OBSERVATION_MAX_AXES + 1U);
    CHECK(!emaster_observation_wire_decode_frame(mutated, written, &decoded, &header),
          "轴数超上限应被拒绝");

    /* frame_bytes 与轴数不自洽 */
    memcpy(mutated, buffer, written);
    mutated[4] = (uint8_t)((written + 8U) & 0xFFU);
    mutated[5] = (uint8_t)(((written + 8U) >> 8U) & 0xFFU);
    CHECK(!emaster_observation_wire_decode_frame(mutated, written, &decoded, &header),
          "frame_bytes 与轴数不自洽应被拒绝");

    /* 截断：长度不足头；长度够头但不够载荷 */
    CHECK(!emaster_observation_wire_decode_frame(buffer, 8U, &decoded, &header),
          "长度不足一个头应被拒绝");
    CHECK(!emaster_observation_wire_decode_frame(buffer, written - 1U, &decoded, &header),
          "长度不足一条完整报文应被拒绝");

    /* 空指针 */
    CHECK(!emaster_observation_wire_decode_frame(NULL, written, &decoded, &header),
          "空缓冲区应被拒绝");
    CHECK(!emaster_observation_wire_decode_frame(buffer, written, NULL, &header),
          "空输出指针应被拒绝");

    /* 只解头时同样要拒绝畸形输入 */
    CHECK(!emaster_observation_wire_decode_header(buffer, 8U, &header),
          "只解头时长度不足也应被拒绝");
    CHECK(emaster_observation_wire_decode_header(buffer, written, &header), "合法头应被接受");
    CHECK(header.axis_count == TEST_AXES, "头里的轴数不对");

    /* 载荷区多出的字节不影响解码：DUMP 一次回多帧时要求能按 frame_bytes 推进。 */
    {
        size_t tail = written + 16U;

        memcpy(mutated, buffer, written);
        memset(&mutated[written], 0xAB, 16U);
        CHECK(emaster_observation_wire_decode_frame(mutated, tail, &decoded, &header),
              "载荷后有多余字节时应仍能解出当前帧");
        CHECK(header.frame_bytes == written, "frame_bytes 与推进长度不一致");
    }
}

/*
 * DUMP 事务头。
 *
 * 这一节存在的理由是一个真出过的故障：没有事务头时，客户端读完最后一帧无从知道流已经
 * 结束，只能继续等下一帧——而观测连接是长连接，服务端不关，于是它永远等下去。所以这里
 * **把"帧数由头给出"当成协议的正常路径来测**，特别是 frame_count 为 0 的空窗口：
 * 它不是错误分支，是最常见的一种回答，而且正是过去会退化成一行文本、让客户端卡死的那条。
 */
static void test_wire_dump_header(void)
{
    uint8_t buffer[256];
    uint8_t mutated[256];
    emaster_observation_wire_dump_t dump;
    size_t written = 0U;

    /* 空窗口：必须编码成功，且帧数为 0 —— 服务端靠它结束一次没有数据的 DUMP。 */
    CHECK(emaster_observation_wire_encode_dump(1234U, 0U, TEST_AXES, buffer, sizeof(buffer),
                                               &written),
          "空窗口的 DUMP 头应能编码");
    CHECK(written == EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES, "DUMP 头长度不对");
    CHECK(emaster_observation_wire_decode_dump(buffer, written, &dump), "DUMP 头应能解码");
    CHECK(dump.frame_count == 0U, "空窗口的帧数应为 0");
    CHECK(dump.first_index == 1234U, "first_index 未原样带回");
    CHECK(dump.axis_count == TEST_AXES, "DUMP 头的轴数不对");
    CHECK(dump.frame_bytes == emaster_observation_wire_frame_bytes(TEST_AXES),
          "DUMP 头报的帧长与轴数不自洽");

    /* 满窗口：帧数上限那一档必须接受（服务端会把 count 钳到这里）。 */
    CHECK(emaster_observation_wire_encode_dump(0U, EMASTER_OBSERVATION_WIRE_MAX_DUMP_FRAMES,
                                               TEST_AXES, buffer, sizeof(buffer), &written),
          "帧数等于上限时应能编码");
    CHECK(emaster_observation_wire_decode_dump(buffer, written, &dump) &&
              dump.frame_count == EMASTER_OBSERVATION_WIRE_MAX_DUMP_FRAMES,
          "帧数等于上限时应能解码");

    /* 超过上限：宁可拒绝，也不发一个客户端会照着追下去的数字。 */
    CHECK(!emaster_observation_wire_encode_dump(0U, EMASTER_OBSERVATION_WIRE_MAX_DUMP_FRAMES + 1U,
                                                TEST_AXES, buffer, sizeof(buffer), &written),
          "帧数超过上限时应拒绝编码");

    /* 容量不足不能写坏调用者的缓冲。 */
    CHECK(!emaster_observation_wire_encode_dump(0U, 0U, TEST_AXES, buffer,
                                                EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES - 1U,
                                                &written),
          "容量不足时应拒绝编码");
    CHECK(!emaster_observation_wire_encode_dump(0U, 0U, TEST_AXES, NULL, sizeof(buffer),
                                                &written),
          "空缓冲指针应被拒绝");

    /* 畸形输入逐项拒绝。每次都从一枚合法的头出发，只改一处。 */
    CHECK(emaster_observation_wire_encode_dump(7U, 3U, TEST_AXES, mutated, sizeof(mutated),
                                               &written),
          "基准 DUMP 头应能编码");
    CHECK(emaster_observation_wire_decode_dump(mutated, written, &dump) && dump.frame_count == 3U,
          "基准 DUMP 头应能解码");

    {
        size_t length = written;

        /* 长度不足。 */
        CHECK(!emaster_observation_wire_decode_dump(mutated, length - 1U, &dump),
              "长度不足时应拒绝解码");

        /* 魔数改一个字节。 */
        memcpy(buffer, mutated, length);
        buffer[0] = (uint8_t)'X';
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump), "魔数错应被拒绝");

        /* 版本 +1。 */
        memcpy(buffer, mutated, length);
        buffer[2] = (uint8_t)(EMASTER_OBSERVATION_WIRE_VERSION + 1U);
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump), "版本不认识应被拒绝");

        /* 类型改成 FRAME：事务头与帧不能互认。 */
        memcpy(buffer, mutated, length);
        buffer[3] = (uint8_t)EMASTER_OBSERVATION_WIRE_KIND_FRAME;
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump), "类型错应被拒绝");

        /* header_bytes 小于本版本：说明是别的东西冒充事务头。 */
        memcpy(buffer, mutated, length);
        buffer[4] = (uint8_t)(EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES - 1U);
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump),
              "header_bytes 过小应被拒绝");

        /* header_bytes 大于实际长度：照着它跳会跑到缓冲外面去。 */
        memcpy(buffer, mutated, length);
        buffer[4] = (uint8_t)(length + 1U);
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump),
              "header_bytes 超过实际长度应被拒绝");

        /* 轴数超上限。 */
        memcpy(buffer, mutated, length);
        buffer[6] = (uint8_t)(EMASTER_OBSERVATION_MAX_AXES + 1U);
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump),
              "轴数超上限应被拒绝");

        /* frame_bytes 与轴数不一致：客户端要按它分配读取量，不能只信这一个字段。 */
        memcpy(buffer, mutated, length);
        buffer[20] = (uint8_t)((buffer[20] + 1U) & 0xFFU);
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump),
              "frame_bytes 与轴数不自洽应被拒绝");

        /* 帧数超过环形容量。 */
        memcpy(buffer, mutated, length);
        buffer[16] = 0xFFU;
        buffer[17] = 0xFFU;
        buffer[18] = 0xFFU;
        buffer[19] = 0xFFU;
        CHECK(!emaster_observation_wire_decode_dump(buffer, length, &dump),
              "帧数超过容量应被拒绝");
    }
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    printf("观测通道离线自测（线格式版本 %u）\n\n", (unsigned)EMASTER_OBSERVATION_WIRE_VERSION);

    test_ring_windows();
    test_ring_concurrency();
    test_writer_independence();
    test_slow_seqlock();
    test_wire_roundtrip();
    test_wire_malformed();
    test_wire_dump_header();

    printf("\n%d 项断言，%d 项失败\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        printf("失败\n");
        return 1;
    }
    printf("通过\n");
    return 0;
}
