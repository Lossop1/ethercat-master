#include "emaster/observation/slow.h"

#include <string.h>

/*
 * 读者允许的尝试次数。观测线程的发布频率是 ~20 Hz，读者（停机后的报告回填）被换出
 * 几十毫秒才会连输两次。再重试只是在收尾阶段空转。
 */
#define EMASTER_OBSERVATION_SLOW_READ_ATTEMPTS 2U

void emaster_observation_slow_init(emaster_observation_slow_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    atomic_init(&snapshot->sequence, UINT32_C(0));
}

void emaster_observation_slow_publish(emaster_observation_slow_t *snapshot,
                                      const emaster_observation_slow_state_t *state)
{
    emaster_observation_slow_state_t staging;
    uint32_t sequence;
    uint64_t valid_mask = UINT64_C(0);
    uint32_t axis_index;

    if (snapshot == NULL || state == NULL)
    {
        return;
    }

    staging = *state;
    if (staging.axis_count > (uint32_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        staging.axis_count = (uint32_t)EMASTER_OBSERVATION_MAX_AXES;
    }
    /* valid_mask 由这里算：让调用方自己维护位图和 axes[].valid 迟早会分叉。 */
    for (axis_index = 0U; axis_index < staging.axis_count; ++axis_index)
    {
        if (staging.axes[axis_index].valid)
        {
            valid_mask |= UINT64_C(1) << axis_index;
        }
    }
    staging.valid_mask = valid_mask;

    sequence = atomic_load_explicit(&snapshot->sequence, memory_order_relaxed);

    /* 奇数 = "正在写"。读者看到奇数就重试，不会拿它当一致版本。 */
    atomic_store_explicit(&snapshot->sequence, sequence + UINT32_C(1), memory_order_relaxed);
    /*
     * 写侧的屏障用 seq_cst，不用 release。release 只保证"先前的写"不越过"其后的
     * 原子写"，管不住这个序号存被下沉到载荷写之后——那样读者会看到一个偶数的、
     * 却指向半写状态的序号，撕裂检查就失效了。这里需要的是彻底的 store-store 序。
     * 代价只有每个快照一次 dmb，而这个写者不是实时线程。
     */
    atomic_thread_fence(memory_order_seq_cst);

    /*
     * 载荷是普通（非原子）访问。按 C11 的字面定义，读者与自己不同步地读同一块内存
     * 构成数据竞争——seqlock 的惯例就是接受这一点，代价用版本号检查来付：读者读到
     * 的版本号必须前后一致，否则整块丢弃。写成逐个 relaxed 原子访问并不能消除竞争
     * （axes 数组没法逐个原子化），只会让代码看起来比它实际做到的更严格。
     * TSan 若在这几行报告竞争，抑制范围就是本函数与 read() 的载荷拷贝。
     */
    snapshot->state.cycle = staging.cycle;
    snapshot->state.monotonic_ns = staging.monotonic_ns;
    snapshot->state.axis_count = staging.axis_count;
    snapshot->state.read_count = staging.read_count;
    snapshot->state.valid_mask = staging.valid_mask;
    memcpy(snapshot->state.axes, staging.axes, sizeof(staging.axes));

    /* 偶数 = "写完了"。release 使上面全部载荷对该序号的读者可见。 */
    atomic_store_explicit(&snapshot->sequence, sequence + UINT32_C(2), memory_order_release);
}

bool emaster_observation_slow_read(const emaster_observation_slow_t *snapshot,
                                   emaster_observation_slow_state_t *out)
{
    unsigned attempt;

    if (snapshot == NULL || out == NULL)
    {
        return false;
    }

    for (attempt = 0U; attempt < EMASTER_OBSERVATION_SLOW_READ_ATTEMPTS; ++attempt)
    {
        uint32_t before = atomic_load_explicit(&snapshot->sequence, memory_order_acquire);
        emaster_observation_slow_state_t staging;
        uint32_t after;

        if ((before & UINT32_C(1)) != 0U)
        {
            /* 写者正在中途。等它写完再来，别读一个明知道是半截的状态。 */
            continue;
        }

        /*
         * acquire 保证下面的载荷读不会上浮到这次序号读之前；因此这里读到的载荷
         * 至少是 `before` 对应的那一版。
         */
        staging.cycle = snapshot->state.cycle;
        staging.monotonic_ns = snapshot->state.monotonic_ns;
        staging.axis_count = snapshot->state.axis_count;
        staging.read_count = snapshot->state.read_count;
        staging.valid_mask = snapshot->state.valid_mask;
        memcpy(staging.axes, snapshot->state.axes, sizeof(staging.axes));

        /*
         * 载荷读不许下沉到这次序号读之后，否则"序号没变"就说明不了"载荷没被改过"。
         * 这是写侧那道 seq_cst 屏障的读者对偶。
         */
        atomic_thread_fence(memory_order_seq_cst);
        after = atomic_load_explicit(&snapshot->sequence, memory_order_relaxed);
        if (after == before)
        {
            *out = staging;
            return true;
        }
    }

    return false;
}
