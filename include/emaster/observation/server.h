#ifndef EMASTER_OBSERVATION_SERVER_H
#define EMASTER_OBSERVATION_SERVER_H

/*
 * 观测通道的传输侧：第二个 AF_UNIX 套接字，**只读**，供外部控制器（神经网络策略）
 * 按自己的频率拉取带时标的原子观测快照。
 *
 * 为什么与命令套接字分开而不是复用：
 *
 * - 权限与风险不同。命令通道能改目标位置，任何能连上它的人都能让电机动；观测通道
 *   只能读。合成一条会让"想看一眼状态"和"能控制电机"变成同一个权限。
 * - 生命周期不同。命令通道必须在周期线程内被轮询（它入队的东西要喂给协调器），
 *   观测通道完全在周期线程之外，只有读者付代价。
 *
 * 为什么是拉取而不是推送：无状态、无积压、无每客户端的速率状态。慢消费者只是拿到
 * 过期的窗口（且被显式标注 STALE），不会在主站侧攒出队列。推送模式要回答"推多快、
 * 客户端跟不上怎么办"，而这两个问题的答案都会让主站的行为取决于客户端。
 *
 * **客户端不读永远无法反压任何东西**：写走 MSG_DONTWAIT 且重试有上界，用尽即关闭该
 * 客户端连接。这是"观测不扰动控制"在传输层的兑现——不只是"周期线程不参与"，而是
 * 整条链路上不存在任何一个会等客户端的阻塞点。
 *
 * v1 单客户端：接受新连接时替换掉前一个（同一模型见 command_server.c）。多客户端
 * 需要每连接一份读位置，而它们读的是同一个只读环形缓冲，收益有限、状态变多。
 *
 * 本模块不依赖 SOEM：它只认环形缓冲和一份静态元数据，因此在开发机与 CI 上都能
 * 完整跑通（含"连上就不读"的对抗用例）。这是把 observation 拆成独立库的主要理由。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emaster/observation/ring.h"
#include "emaster/observation/wire.h"

/*
 * DUMP 的载荷**不是**另一种格式，而是 emaster_observation_wire_encode_frame() 的输出的
 * 直接拼接——每帧自带魔数、版本与 frame_bytes，自描述。
 *
 * 为什么不自造一个"多帧事务头"：那会在这个库里引入第二种线格式，而两种格式意味着
 * 两处偏移、两处版本号、两套客户端解析。已有的编解码器（wire.h）已经把"布局变更可
 * 检出"这件事做完了，多一层只是多一层能对不上的地方。
 *
 * 客户端因此按 decode_header 逐帧推进即可，与解单帧是同一条代码路径。
 *
 * "窗口缺没缺"不需要事务头回答：publish_index 每发布一帧加一，所以客户端拿到的一串
 * 帧如果 publish_index 连续，就是无洞的；一旦跳号，跳了多少一目了然。这比在响应头里
 * 附带一个"回帧时的 head"更准——那个值在传输期间就已经过期了。
 */

/* 单个 DUMP 请求允许的最大帧数（= 环形缓冲容量，超出无意义）。 */
#define EMASTER_OBSERVATION_MAX_DUMP_FRAMES EMASTER_OBSERVATION_RING_CAPACITY

/*
 * 客户端解析 INFO 所需的静态信息。这些值在一次会话内不变，所以走 INFO 下发一次，
 * 不随每帧重复携带（stride/轴数/容量都是部署常量）。
 */
typedef struct
{
    /* 部署 ID，供客户端确认自己连的是哪个主站。 */
    char deployment_id[64];
    /* EtherCAT 网口名，纯标识用。 */
    char interface_name[32];
    uint16_t axis_count;
    /* cycle 的增量语义，通常为 1。缺口拍数 = (Δcycle - stride) / stride。 */
    uint16_t stride;
    uint32_t ring_capacity;
} emaster_observation_info_t;

typedef struct emaster_observation_server emaster_observation_server_t;

/*
 * 建立观测套接字并启动监听线程。成功返回句柄，失败返回 NULL。
 *
 * `ring` 的所有权不转移，且必须比本服务器活得久——服务器线程会一直读它。
 * 调用方（会话）必须先销毁本服务器再释放环形缓冲。
 */
emaster_observation_server_t *
emaster_observation_server_create(const char *socket_path,
                                  const emaster_observation_ring_t *ring,
                                  const emaster_observation_info_t *info);

/* 停止监听线程、关闭连接、unlink 套接字文件并释放。NULL 安全，可重复调用。 */
void emaster_observation_server_destroy(emaster_observation_server_t *server);

/* 实际绑定的路径，未成功建立时返回 NULL。 */
const char *emaster_observation_server_socket_path(const emaster_observation_server_t *server);

#endif /* EMASTER_OBSERVATION_SERVER_H */
