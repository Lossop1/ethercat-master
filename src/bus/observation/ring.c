#include "emaster/observation/ring.h"

#include <string.h>

/*
 * 读者允许的尝试次数。第一次失败通常意味着写者刚刚越过本槽；第二次仍失败说明读者
 * 被换出了整整一圈（1 ms × 256 = 256 ms），继续重试只会让读者在实时系统边上空转，
 * 不如老实地报 UNSTABLE 让上层决定。
 */
#define EMASTER_OBSERVATION_READ_ATTEMPTS 2U

/*
 * 可读窗口的下界。**这不是"最老的一帧"，是"最老的一帧之后那一帧"。**
 *
 * 为什么最老的一帧不能读——这是本文件唯一的非平凡不变式，改发布协议前先读这段。
 *
 * publish() 的结构是：relaxed 读 head 得到本次的发布序号 w（此时 head == w）→ 写槽
 * w % CAP → release 存 head = w+1。于是"正在被写的那一格槽"永远是
 * `head % CAP`：写者要么还没开始（下一次发布就会落在 head % CAP），要么正写到一半
 * （w == head）。
 *
 * 读者请求序号 i 时，槽 i % CAP 会被两次发布写过：第 i 次和第 i+CAP 次。而
 * `head 没变` 只能排除"这两次发布在上次读 head 与这次读 head 之间**完成**"，
 * 排除不了**正在写入**：写者写了一半槽、还没执行那次 release 存，head 就原封不动。
 * 此时读者读到的是一半旧帧、一半新帧的混合物，两道判据都会认为它没问题。
 *
 * 唯一的交集是 i == head - CAP（此时 i + CAP == head，正是写者手上的那一格）。
 * 把下界抬到 head - CAP + 1，就可读槽与在写槽永久错开，**写者不需要为此多花一条
 * 指令**——不需要每槽序号、不需要额外的栅栏。代价是环形缓冲少存一帧。
 *
 * 依赖的前提：写者只有一个，且 release 存 head 之前不再读它、不 CAS、不重试。
 * 若将来要让多个写者并发发布，这个不变式立刻失效，必须改成每槽序号（seqlock）。
 */
static uint64_t oldest_readable(uint64_t head)
{
    /*
     * 边界是 head >= CAPACITY，不是 >。head 正好等于 CAPACITY 时，hazard 槽是
     * CAPACITY % CAPACITY == 0，正是第 0 帧所在的槽——写成 > 会把它放出去。
     * 这一条是自测 tool 在 head=CAP 的边界用例上抓出来的，别再改回去。
     */
    return head >= (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY
               ? head - (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY + UINT64_C(1)
               : 0U;
}

void emaster_observation_ring_init(emaster_observation_ring_t *ring)
{
    if (ring == NULL)
    {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    atomic_init(&ring->head, UINT64_C(0));
}

void emaster_observation_ring_publish(emaster_observation_ring_t *ring,
                                      const emaster_observation_frame_t *frame)
{
    emaster_observation_frame_t *slot;
    uint64_t index;
    uint16_t axis_count;

    if (ring == NULL || frame == NULL)
    {
        return;
    }

    /*
     * head 只有写者写，所以 relaxed 读拿到的就是真值——这里不需要屏障，
     * 需要的屏障在下面对 head 的 release 存上。
     */
    index = atomic_load_explicit(&ring->head, memory_order_relaxed);

    /* 超上限时截断而不是报错：实时路径上不做错误处理，少几个轴也比整拍丢掉好。 */
    axis_count = frame->axis_count;
    if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        axis_count = (uint16_t)EMASTER_OBSERVATION_MAX_AXES;
    }

    slot = &ring->slots[(size_t)(index % (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY)];
    *slot = *frame;
    slot->axis_count = axis_count;
    /*
     * publish_index 是槽内**最后**被写的字段，读者靠它做覆盖校验，所以必须在这里
     * 赋而不是让调用者填。它也是读者唯一的"这槽装的是第几帧"依据。
     */
    slot->publish_index = index;

    /*
     * 第一道屏障。release 保证上面所有对槽的写入对 acquire 到这个新 head 的读者可见。
     * 不 CAS、不自旋、不重试——写者执行的指令与读者的存在无关，这是"观测不扰动控制"
     * 的机制保证。读者能施加的唯一影响是这一行缓存的所有权争用，有界且不构成等待。
     */
    atomic_store_explicit(&ring->head, index + UINT64_C(1), memory_order_release);

    /*
     * 第二道屏障，**方向与上一道相反**：把这次 head 存钉在**下一次**发布的载荷存之前。
     *
     * release 只管"先前的写"不越过它，管不住"其后的写"提前——而这里要防的恰恰是后者：
     * 发布 index+CAP 的载荷存，可以在 head = index+CAP 那次存之前就被读者观察到。于是
     * 读者会看到下一圈的载荷、却读到还没变的 head，两道判据一齐放行。自测在 aarch64
     * 上抓到过正是这一形态（撕裂帧的 head 恰好等于 index+CAP，载荷却不是本圈）。
     *
     * 少了它，读者的 acquire 屏障只能挡住"读者自己把载荷读重排到复查之后"，挡不住
     * "写者把 head 存落在了载荷存之后"。两者是不同的缺口，各要各的屏障。
     *
     * C11 里对应"先前的访问先于其后的写"的栅栏就是 release 栅栏。代价：aarch64 上一条
     * dmb ish，x86 上是空（TSO 已经保证 store-store 序，编译器不会为此发指令）。
     * 仍然与读者无关——这条屏障的耗时不由任何读者的行为决定。
     */
    atomic_thread_fence(memory_order_release);
}

uint64_t emaster_observation_ring_head(const emaster_observation_ring_t *ring)
{
    if (ring == NULL)
    {
        return 0U;
    }
    return atomic_load_explicit(&ring->head, memory_order_acquire);
}

bool emaster_observation_ring_window(const emaster_observation_ring_t *ring,
                                     uint64_t *oldest,
                                     uint64_t *newest)
{
    uint64_t head;
    uint64_t first;

    if (ring == NULL)
    {
        return false;
    }
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (oldest != NULL)
    {
        *oldest = 0U;
    }
    if (newest != NULL)
    {
        *newest = 0U;
    }
    if (head == 0U)
    {
        return false;
    }
    first = oldest_readable(head);
    if (oldest != NULL)
    {
        *oldest = first;
    }
    if (newest != NULL)
    {
        *newest = head;
    }
    return true;
}

emaster_observation_read_status_t
emaster_observation_ring_read(const emaster_observation_ring_t *ring,
                              uint64_t index,
                              emaster_observation_frame_t *out_frame)
{
    unsigned attempt;

    if (ring == NULL || out_frame == NULL)
    {
        return EMASTER_OBSERVATION_READ_UNSTABLE;
    }

    for (attempt = 0U; attempt < EMASTER_OBSERVATION_READ_ATTEMPTS; ++attempt)
    {
        uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
        uint64_t oldest;
        const emaster_observation_frame_t *slot;
        uint64_t recheck;

        if (head == 0U)
        {
            return EMASTER_OBSERVATION_READ_EMPTY;
        }
        oldest = oldest_readable(head);
        if (index < oldest || index >= head)
        {
            /*
             * 已被覆盖、正是那一格"可能正被写者占用"的槽、或尚未发布。三个方向都
             * 不返回数据：半截帧比丢帧更危险。
             */
            return EMASTER_OBSERVATION_READ_STALE;
        }

        slot = &ring->slots[(size_t)(index % (uint64_t)EMASTER_OBSERVATION_RING_CAPACITY)];
        *out_frame = *slot;

        /*
         * 载荷读必须先于下面那次 head 复查**完成**，这一道屏障就是干这个的。
         *
         * 少了它，乱序核会把复查的读提前执行：它命中的还是读者 L1 里那个旧的 head，
         * 于是复查报"没变"，而载荷读落在后面，已经把写者这一遍的写看了进去。结果
         * 就是一道通过了全部判据的撕裂帧——这是 aarch64 上实测到的（x86-TSO 下没露
         * 出来，所以自测在本机是绿的、在 Pi 上是红的）。
         *
         * 缺口只在"读者请求的正好是可读窗口下界"时兑现：那一格的槽恰是写者的**下一**
         * 次发布要写的位置（见 oldest_readable 的推导）。读者请求 index = head - CAP + 1
         * 时，index % CAP == (head + 1) % CAP，写者发布的下一帧正落在这里。
         *
         * 代价在读者侧：aarch64 上一条 dmb ishld。写者那条路径一个字都没动，
         * "写者不需要为此多花一条指令"这个性质仍然成立。
         */
        atomic_thread_fence(memory_order_acquire);

        /*
         * 屏障立起来之后，"head 没变"才真的蕴含"这一遍拷贝期间写者没有完成任何一次
         * 发布"。再叠上 publish_index 的比对，抓的是"槽装错了帧"这类 head 判不出来的
         * 错误（例如序号算术写错）。
         */
        recheck = atomic_load_explicit(&ring->head, memory_order_acquire);
        if (recheck == head && out_frame->publish_index == index)
        {
            return EMASTER_OBSERVATION_READ_OK;
        }
    }

    return EMASTER_OBSERVATION_READ_UNSTABLE;
}

emaster_observation_read_status_t
emaster_observation_ring_read_or_latest(const emaster_observation_ring_t *ring,
                                        uint64_t index,
                                        emaster_observation_frame_t *out_frame,
                                        bool *degraded)
{
    emaster_observation_read_status_t status = emaster_observation_ring_read(ring, index, out_frame);

    if (status == EMASTER_OBSERVATION_READ_OK)
    {
        if (degraded != NULL)
        {
            *degraded = false;
        }
        return EMASTER_OBSERVATION_READ_OK;
    }

    if (status == EMASTER_OBSERVATION_READ_STALE)
    {
        uint64_t head = emaster_observation_ring_head(ring);

        if (head > 0U)
        {
            status = emaster_observation_ring_read(ring, head - UINT64_C(1), out_frame);
        }
    }

    if (status == EMASTER_OBSERVATION_READ_OK && degraded != NULL)
    {
        *degraded = true;
    }
    return status;
}
