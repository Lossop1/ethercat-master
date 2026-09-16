#ifndef EMASTER_CYCLIC_WINDOW_H
#define EMASTER_CYCLIC_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 滑动窗口计数：回答"最近 window_ns 内发生了多少次"，而不是"从会话开始累计多少次"。
 *
 * 存在原因（2026-09-16 台架证据）：WKC 容错的 total_error_threshold 原先按整轮会话累计，
 * 于是一次发生在第 11 秒的孤立整帧缺失会一直挂在账上，等到第 50 次把一次本来健康的
 * 25 分钟会话判死——期间零连续错误、零截止时间超时、跟随误差正常。健康与否的判据必须
 * 是"眼下的错误有多密"，不是"历史上总共错了多少次"，否则长跑的唯一结局就是踩满配额。
 *
 * 实现用定长桶环而不是时间戳环形队列：桶数和每条事件的推进步数都是常数，且**没有事件
 * 时一个周期都不花**——这东西挂在 1 kHz 的周期路径上，不能按周期付代价。调用方只在
 * 真出现错误时调用 record()。
 *
 * 精度：窗口跨度是"当前桶加上之前 N-1 个桶"，因此实际窗口落在
 * [(N-1) * bucket_ns, N * bucket_ns] 之间，误差不超过 1/N（N = 32 时约 3%）。
 * 阈值判定不需要比这更准。
 */

#define EMASTER_CYCLIC_WINDOW_BUCKET_COUNT 32U

typedef struct
{
    /* 各桶的事件数。下标是 bucket_index 对桶数取模，不是时间顺序。 */
    uint64_t buckets[EMASTER_CYCLIC_WINDOW_BUCKET_COUNT];
    /* 桶内合计，增量维护：取值时不必再遍历 32 个桶。 */
    uint64_t total;
    uint64_t window_ns;
    /* window_ns / BUCKET_COUNT，至少 1，保证推进步长的除法不除零。 */
    uint64_t bucket_ns;
    /* 当前桶的序号，自 init 起单调递增，只增不减。 */
    uint64_t bucket_index;
    /* 当前桶的起点时刻。 */
    uint64_t bucket_start_ns;
    /* 第一次 record 之前没有时基，此时不做任何推进。 */
    bool started;
} emaster_cyclic_window_t;

/*
 * 初始化窗口。window_ns 为 0 时桶宽退回 1ns，窗口退化到"几乎只有当下"——配置层
 * 不允许出现 0，这里只是保证结构在任何输入下都可用。
 */
void emaster_cyclic_window_init(emaster_cyclic_window_t *window, uint64_t window_ns);

/*
 * 记一次事件并返回记录之后窗口内的事件数。now_ns 必须来自同一个单调时钟，
 * 且不得回退（回退时不推进，宁可让窗口偏长）。
 */
uint64_t emaster_cyclic_window_record(emaster_cyclic_window_t *window, uint64_t now_ns);

/* 只读取窗口内的事件数，不记账。语义与 record() 返回的一致。 */
uint64_t emaster_cyclic_window_count(emaster_cyclic_window_t *window, uint64_t now_ns);

#endif
