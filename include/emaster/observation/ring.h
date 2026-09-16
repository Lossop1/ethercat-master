#ifndef EMASTER_OBSERVATION_RING_H
#define EMASTER_OBSERVATION_RING_H

/*
 * 单生产者单消费者（SPSC）环形缓冲：周期线程发布，任意非实时线程读取。
 *
 * 设计目标只有一条，但它是硬要求：**读者的行为不得改变控制回路的行为**。
 *
 * 怎么做到：
 *
 * - 写者只做「填槽 → 普通存 publish_index → release 存 head」三步。一次屏障，
 *   不 CAS、不自旋、不重试、不读任何读者可改的位置，也不分配内存。写者走的**代码
 *   路径**与读者完全无关：没有读者、读者读得飞快、读者连上就不读、读者被 kill，
 *   写者执行的指令都一模一样。这是"不扰动控制"的机制保证，不是靠调优先级弥补。
 * - 读者承担全部代价：acquire 读 head → 拷贝槽 → **acquire 屏障** → 再 acquire 读
 *   head 校验 + publish_index 比对，重试有上界。重试用尽时明确报 UNSTABLE，绝不
 *   返回半截帧冒充完整帧。
 *
 * 那道 acquire 屏障不是装饰：少了它，乱序核会把复查的 head 读提前到载荷读之前执行，
 * 读到的是自己 L1 里的旧值，于是复查放行、载荷却已经是写者新写进去的内容。这条在
 * aarch64 上实测可复现（自测的并发读者各读到撕裂帧），在 x86-TSO 上不出现——所以
 * "本机自测是绿的"完全不能说明这条路径没问题。推导见 ring.c 里那道屏障处的注释。
 *
 * 说清楚这个保证**不**包含什么：写者仍然要写 head，那一行缓存被几个核同时读会让
 * 写者拿到独占权的代价上升。这是有界的（实测四个满速自旋读者把单帧发布从 89 ns
 * 抬到 425 ns，见 observation-selftest），量级是缓存行往返，**不是**等待——没有
 * 任何一条路径会让写者停下来等读者。放在 1 ms 周期、单次发布的真实场景里，它落在
 * 周期的千分之一以下；而"读者卡住把周期线程拖死"这种失效模式在设计上不存在。
 *
 * 为什么是环形缓冲而不是 seqlock / 双缓冲：
 *
 * - 单值 seqlock（只发布最新一帧）满足不了需求。50 Hz 的策略每 20 ms 才来取一次，
 *   而周期是 1 ms，latest-value 只有 1 帧可用，根本重构不出 120–200 ms 的历史窗口，
 *   会逼客户端按 1 kHz 轮询——那正是要避免的负载。
 * - 双缓冲要发布历史就得每周期 memcpy 整个历史（≈110 MB/s 的实时线程存储带宽），
 *   严格劣于"每周期存一帧 432 B"。
 *
 * 覆盖语义：槽数固定为 EMASTER_OBSERVATION_RING_CAPACITY。写者不会因为读者落后
 * 而放慢或阻塞，落后超过容量就覆盖。读者请求已被覆盖的序号得到 STALE，**不返回
 * 任何数据**——半截帧比丢帧更危险。
 *
 * 可读窗口是 [head - CAPACITY + 1, head)：**最老的那一帧不可读**。原因是它与写者
 * 当前正在写的那一格槽是同一个物理槽，而"head 没变"这个判据排除不了"写者写了一半
 * 还没存 head"。完整推导见 ring.c 的 oldest_readable()。代价是历史窗口少一帧，
 * 换来写者不必额外加每槽序号或栅栏。
 *
 * 内存序（C11 <stdatomic.h>）：aarch64/LP64 上 _Atomic uint64_t 是无锁的。
 * 写者的 release 存与读者的 acquire 读配对，保证读者看到 head 增量时，
 * 该槽的载荷已全部可见。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emaster/observation/frame.h"

/*
 * 256 槽 @ 1 ms = 256 ms 历史。覆盖策略观测窗口的最坏情况（200 ms），
 * 并留出消费者被换出的余量。增大它只是按比例增大常驻内存（每槽 432 B，
 * 256 槽共 108 KiB），不改变任何语义。
 */
#define EMASTER_OBSERVATION_RING_CAPACITY 256U

/*
 * 读请求的结局。
 *
 * 只有 OK 时 out_frame 的内容保证自洽（同一拍、无撕裂）。其余状态下一律不写
 * out_frame，调用者不得读取它。
 */
typedef enum
{
    EMASTER_OBSERVATION_READ_OK = 0,
    /* 还没有任何帧发布过（写者尚未启动）。 */
    EMASTER_OBSERVATION_READ_EMPTY,
    /* 请求的序号不在可读窗口内：已被覆盖、尚未发布，或是那一格最老且可能正被
     * 写者占用的槽。见 ring.c 的 oldest_readable()。 */
    EMASTER_OBSERVATION_READ_STALE,
    /* 写者在本函数两遍读 head 之间越过了本槽，重试用尽。 */
    EMASTER_OBSERVATION_READ_UNSTABLE
} emaster_observation_read_status_t;

/*
 * 环形缓冲。写者与读者各用各的字段，没有任何共享的可写位置——head 是唯一的
 * 跨线程量，且只有写者写。
 */
typedef struct
{
    /* 已发布的帧数，兼作下一个发布序号。只有写者写，读者只 acquire 读。 */
    _Atomic uint64_t head;
    /* 显式填充，避免 head 与首个槽共享缓存行造成伪共享。 */
    uint64_t _pad[7];
    /* 每槽的载荷 + 该槽当前的发布序号。槽内最后一个被写的字段就是 publish_index。 */
    emaster_observation_frame_t slots[EMASTER_OBSERVATION_RING_CAPACITY];
} emaster_observation_ring_t;

/* 归零。必须在任何线程启动前调用；并发调用与使用是未定义行为。 */
void emaster_observation_ring_init(emaster_observation_ring_t *ring);

/*
 * 发布一帧。**周期线程专用**，wait-free，不做系统调用、不分配、不读共享可写状态。
 *
 * frame->publish_index 由本函数填充，调用者不必（也不应）自己设。
 * frame->axis_count 超过 EMASTER_OBSERVATION_MAX_AXES 时按上限截断——宁可少几个轴，
 * 也不要在实时路径上做错误处理。
 */
void emaster_observation_ring_publish(emaster_observation_ring_t *ring,
                                      const emaster_observation_frame_t *frame);

/* 当前 head（已发布帧数）。读者用它算出"最新一帧的序号 = head - 1"。 */
uint64_t emaster_observation_ring_head(const emaster_observation_ring_t *ring);

/*
 * 取可读窗口 [*oldest, *newest) 的边界，均为合法的可请求序号。
 * 缓冲区为空时两者都置 0 并返回 false。读者在决定"要哪几帧"之前先问这个，
 * 可以避免用 STALE 当流程控制。
 */
bool emaster_observation_ring_window(const emaster_observation_ring_t *ring,
                                     uint64_t *oldest,
                                     uint64_t *newest);

/*
 * 读取指定发布序号的帧。多读者安全（读者不写任何共享状态）。
 * 返回 OK 时 out_frame 自洽；其余状态不写 out_frame。
 */
emaster_observation_read_status_t
emaster_observation_ring_read(const emaster_observation_ring_t *ring,
                              uint64_t index,
                              emaster_observation_frame_t *out_frame);

/*
 * 读取指定序号；若已被覆盖则**退回最新一帧**并把 *degraded 置 true。
 *
 * 与 read() 的区别只在 STALE 的处理上。观测的消费者是策略，它宁可拿到一帧旧的、
 * 被明确标注降级的观测，也不愿因为"自己跑慢了"而整帧丢弃、在历史窗口里留洞——
 * 洞会静默改变策略看到的时序。所以这一档是给消费者用的，read() 是给需要严格
 * 语义的工具用的。
 *
 * *degraded 是否被写：仅在返回 OK 时写入；返回 EMPTY/UNSTABLE 时不碰。
 */
emaster_observation_read_status_t
emaster_observation_ring_read_or_latest(const emaster_observation_ring_t *ring,
                                        uint64_t index,
                                        emaster_observation_frame_t *out_frame,
                                        bool *degraded);

#endif /* EMASTER_OBSERVATION_RING_H */
