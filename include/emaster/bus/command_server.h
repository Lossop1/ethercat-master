#ifndef EMASTER_BUS_COMMAND_SERVER_H
#define EMASTER_BUS_COMMAND_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 实时命令服务器：通过Unix域套接字接收运行时命令。
 * 设计目标：
 * - 非阻塞：不影响实时周期
 * - 双向通信：支持命令响应
 * - 线程安全：命令队列与周期线程解耦
 */

typedef struct emaster_command_server emaster_command_server_t;

/* 命令类型：定义主站运行期间支持的操作 */
typedef enum {
    EMASTER_COMMAND_INVALID = 0,
    EMASTER_COMMAND_QUERY_STATUS,        /* 查询当前状态 */
    EMASTER_COMMAND_QUERY_TOPOLOGY,      /* 查询拓扑和参数 */
    EMASTER_COMMAND_SWITCH_MOTION,       /* 切换运动轨迹 */
    EMASTER_COMMAND_STOP_MOTION,         /* 停止运动 */
    EMASTER_COMMAND_SET_EXTERNAL_TARGET, /* 设置外部位置目标 */
    EMASTER_COMMAND_SHUTDOWN,            /* 请求优雅关闭 */
    EMASTER_COMMAND_QUICK_STOP,          /* 触发全轴 Quick Stop（控制字 0x0002） */
    EMASTER_COMMAND_HALT,                /* 设置全轴 Halt 位（控制字 bit8），payload: "1"=置位 "0"=清零 */
    EMASTER_COMMAND_FAULT_RESET,         /* 触发全轴 Fault Reset（控制字 0x0080） */
    EMASTER_COMMAND_RECOVER_AXIS,        /* P2.5: 恢复单个故障轴，payload: 轴索引 */
} emaster_command_type_t;

/* 命令结构：封装命令类型和参数 */
typedef struct {
    emaster_command_type_t type;
    char payload[256];  /* 命令参数（JSON字符串或纯文本） */
} emaster_command_t;

/* 命令响应结构。message 需容纳多轴状态回读，按轴数与字段数留足余量。 */
typedef struct {
    bool success;
    char message[1024];
} emaster_command_response_t;

/*
 * P9.4: 命令流量的定长记账。
 *
 * 此前命令服务器对每条命令打两行 stderr（"Parsed"+"Enqueued"），100 Hz 的外部目标流
 * 就是 200 行/秒；一次 180 s 的五轴往返因此产出 1.1 MB 日志，而"命令到底有没有在流"
 * 却在报告里没有落点。改为：逐条明细退到 EMASTER_CMD_SERVER_VERBOSE 后面，计数进报告。
 *
 * 三个计数互斥且穷尽一条命令的三种去向，相加等于服务器收到过的全部字节组。
 */
typedef struct {
    uint64_t received_count;   /* 解析成功并入队的命令数 */
    uint64_t invalid_count;    /* 认不出类型的请求（客户端 bug 或协议版本不一致） */
    uint64_t queue_full_count; /* 入队时队列已满而被丢弃的命令数 */
} emaster_command_server_stats_t;

/*
 * 创建命令服务器。
 * socket_path: Unix域套接字路径（例如："/tmp/emaster-cmd.sock"）
 * 返回: 服务器实例，失败返回NULL
 */
emaster_command_server_t *emaster_command_server_create(const char *socket_path);

/*
 * 销毁命令服务器并清理资源。
 *
 * stats 非空时把最终计数写进去（可为 NULL）。取数是线程 join 之后做的，所以拿到的是
 * 完整值而不是某一瞬间的快照——调用方不必再自己同步。
 */
void emaster_command_server_destroy(emaster_command_server_t *server,
                                    emaster_command_server_stats_t *stats);

/*
 * 非阻塞地接收一个命令。
 * 返回: true表示接收到命令，false表示无命令
 * 线程安全：可从周期线程调用
 */
bool emaster_command_server_receive(emaster_command_server_t *server,
                                   emaster_command_t *command);

/*
 * 发送命令响应。
 * 线程安全：可从周期线程调用
 */
bool emaster_command_server_respond(emaster_command_server_t *server,
                                   const emaster_command_response_t *response);

/*
 * 获取套接字路径（用于客户端连接）。
 */
const char *emaster_command_server_socket_path(const emaster_command_server_t *server);

#endif
