#ifndef EMASTER_BUS_COMMAND_SERVER_H
#define EMASTER_BUS_COMMAND_SERVER_H

#include <stdbool.h>
#include <stddef.h>

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
    EMASTER_COMMAND_QUERY_STATUS,      /* 查询当前状态 */
    EMASTER_COMMAND_QUERY_TOPOLOGY,    /* 查询拓扑和参数 */
    EMASTER_COMMAND_SWITCH_MOTION,     /* 切换运动轨迹 */
    EMASTER_COMMAND_STOP_MOTION,       /* 停止运动 */
    EMASTER_COMMAND_SET_EXTERNAL_TARGET, /* 设置外部位置目标 */
    EMASTER_COMMAND_SHUTDOWN           /* 请求优雅关闭 */
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
 * 创建命令服务器。
 * socket_path: Unix域套接字路径（例如："/tmp/emaster-cmd.sock"）
 * 返回: 服务器实例，失败返回NULL
 */
emaster_command_server_t *emaster_command_server_create(const char *socket_path);

/*
 * 销毁命令服务器并清理资源。
 */
void emaster_command_server_destroy(emaster_command_server_t *server);

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
