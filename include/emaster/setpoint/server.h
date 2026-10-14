#ifndef EMASTER_SETPOINT_SERVER_H
#define EMASTER_SETPOINT_SERVER_H

/*
 * 设点通道的传输侧：第三个 AF_UNIX 套接字，**只进不出**，供外部控制器（第一个真实
 * 场景是 rl_sar）按自己的频率送目标。
 *
 * 与前两个通道的分工：
 *   - 命令口 /tmp/emaster-<id>.sock：文本、可读可写、影响控制（含模式切换等低频动作）。
 *   - 观测口 /tmp/emaster-<id>-obs.sock：二进制、只读，纯旁路。
 *   - 设点口 /tmp/emaster-<id>-sp.sock：二进制、**只写**，周期路径唯一的输入。
 *
 * 为什么另开一条而不是在命令口上加二进制动词：命令口是文本、带响应、按行分帧，客户端
 * 必须与主站同频读回响应。设点要的是"客户端按自己的频率投，投完就走"，任何一处会等
 * 客户端的阻塞点都会变成周期路径上的抖动源。这条通道没有任何回写——全链路上不存在
 * 一个"主站等客户端"的点。
 *
 * 为什么用 SOCK_SEQPACKET：消息边界由内核给。粘包、拆包、半条记录这一整类问题不存在，
 * 因此这里不需要命令口那种跨 read 的组帧缓冲，也不需要观测口那种行扫描。
 *
 * v1 单客户端：接受新连接即替换旧的（与另两口同模型）。多客户端需要回答"两个客户端
 * 谁是权威"，而那是部署问题，不是这个模块该猜的。
 *
 * 本模块不依赖 SOEM。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emaster/motion/external_target_buffer.h"
#include "emaster/setpoint/wire.h"

/*
 * 通道的累计事实，收尾时取一次写进报告。
 *
 * 都是"发生了什么"的计数，不是"应该发生什么"的期望：主站不假设客户端频率，也就
 * 不报"应该收到多少条"。丢没丢包由 sequence_gaps 回答，客户端是否还活着由
 * last_record_age_ns 回答。
 */
typedef struct
{
    bool     enabled;
    uint64_t records_accepted;
    uint64_t records_rejected;
    /* 相邻两条已采纳记录之间跳过的序号总数。序号回退或重复不计。 */
    uint64_t sequence_gaps;
    uint64_t last_sequence;
    uint64_t last_client_time_ns;
    /* 收尾时最后一条已采纳记录有多旧（纳秒）；从未收到过为 -1。 */
    int64_t  last_record_age_ns;
    emaster_setpoint_reject_t last_reject_reason;
    /* 最后一次被拒的轴号（0 起）；与原因无关时为 0。 */
    size_t   last_reject_axis;
} emaster_setpoint_stats_t;

typedef struct emaster_setpoint_server emaster_setpoint_server_t;

/*
 * 建套接字并起监听线程。成功返回句柄，失败返回 NULL。
 *
 * buffer 的所有权不转移、且必须比服务器活得久——监听线程会一直写它。它同时被周期
 * 回调读取，两者用 buffer 自己的互斥锁同步。
 *
 * expected_modes 是逐轴的期望运行模式（8/9/10），来自会话计划；某轴取
 * EMASTER_SETPOINT_MODE_UNCHECKED 时该轴不核对模式。数组长度为 axis_count。
 */
#define EMASTER_SETPOINT_MODE_UNCHECKED ((int8_t)-1)

emaster_setpoint_server_t *
emaster_setpoint_server_create(const char *socket_path,
                               emaster_external_target_buffer_t *buffer,
                               size_t axis_count,
                               const int8_t *expected_modes);

/*
 * 停线程、取累计事实、关连接、unlink 套接字文件并释放。NULL 安全。
 *
 * 统计与销毁合成一步，是为了让报告里的数字**就是整轮的**：线程停掉之后再取，此后
 * 不可能再有记录到达，也不存在"读的时候监听线程正在加一"这种窗口。分成两个调用
 * 就得再引入一把锁去补同一个洞。
 *
 * stats 为 NULL 时只销毁。server 为 NULL 时写出一份 enabled=false 的空统计。
 */
void emaster_setpoint_server_destroy(emaster_setpoint_server_t *server,
                                     emaster_setpoint_stats_t *stats);

/* 实际绑定的路径，未成功建立时返回 NULL。 */
const char *emaster_setpoint_server_socket_path(const emaster_setpoint_server_t *server);

#endif /* EMASTER_SETPOINT_SERVER_H */
