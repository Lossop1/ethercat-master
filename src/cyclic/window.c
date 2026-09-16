#include "emaster/cyclic/window.h"

#include <string.h>

/*
 * 把时基推到 now_ns 所处的桶，并把滑出窗口的桶清空、从合计里减掉。
 *
 * 只在有事件时被调用，因此这里的循环哪怕最坏走满 EMASTER_CYCLIC_WINDOW_BUCKET_COUNT 步
 * 也不影响周期路径的正常时序——没有错误时这个文件一行都不执行。
 */
static void advance_to(emaster_cyclic_window_t *window, uint64_t now_ns)
{
    uint64_t steps;
    uint64_t step;

    if (window->started && now_ns <= window->bucket_start_ns)
    {
        /* 时钟回退或原地不动：不推进。窗口因此偏长，判定偏保守。 */
        return;
    }
    if (window->started)
    {
        steps = (now_ns - window->bucket_start_ns) / window->bucket_ns;
        if (steps == 0U)
        {
            return;
        }
        if (steps >= (uint64_t)EMASTER_CYCLIC_WINDOW_BUCKET_COUNT)
        {
            /* 整个窗口都过期了，连当前桶一起清掉，省去逐桶推进。 */
            memset(window->buckets, 0, sizeof(window->buckets));
            window->total = 0U;
            window->bucket_index += steps;
            window->bucket_start_ns += steps * window->bucket_ns;
            return;
        }
        for (step = 1U; step <= steps; ++step)
        {
            size_t index = (size_t)((window->bucket_index + step) %
                                    (uint64_t)EMASTER_CYCLIC_WINDOW_BUCKET_COUNT);
            window->total -= window->buckets[index];
            window->buckets[index] = 0U;
        }
        window->bucket_index += steps;
        window->bucket_start_ns += steps * window->bucket_ns;
        return;
    }
    window->started = true;
    window->bucket_index = 0U;
    window->bucket_start_ns = now_ns;
}

void emaster_cyclic_window_init(emaster_cyclic_window_t *window, uint64_t window_ns)
{
    if (window == NULL)
    {
        return;
    }
    memset(window, 0, sizeof(*window));
    window->window_ns = window_ns;
    window->bucket_ns = window_ns / (uint64_t)EMASTER_CYCLIC_WINDOW_BUCKET_COUNT;
    if (window->bucket_ns == 0U)
    {
        window->bucket_ns = 1U;
    }
}

uint64_t emaster_cyclic_window_record(emaster_cyclic_window_t *window, uint64_t now_ns)
{
    size_t index;

    if (window == NULL)
    {
        return 0U;
    }
    advance_to(window, now_ns);
    index = (size_t)(window->bucket_index % (uint64_t)EMASTER_CYCLIC_WINDOW_BUCKET_COUNT);
    ++window->buckets[index];
    ++window->total;
    return window->total;
}

uint64_t emaster_cyclic_window_count(emaster_cyclic_window_t *window, uint64_t now_ns)
{
    if (window == NULL)
    {
        return 0U;
    }
    advance_to(window, now_ns);
    return window->total;
}
