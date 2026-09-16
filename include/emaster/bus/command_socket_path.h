#ifndef EMASTER_BUS_COMMAND_SOCKET_PATH_H
#define EMASTER_BUS_COMMAND_SOCKET_PATH_H

/*
 * 命令套接字路径的唯一构造点，以及 CLI 工具共用的选项解析。
 *
 * 存在原因：主站按部署 ID 建套接字（/tmp/emaster-<deployment_id>.sock），而各 CLI
 * 工具此前各自写死一份路径字面量——emaster-move/move-deg/watch 三处写死台架双轴，
 * status_client 更是拼成了 /tmp/emaster-orangemaster.sock，永远匹配不上任何部署。
 * 换个部署或改个命名，工具要么连错主站，要么连不上却看不出原因。
 *
 * 因此格式串只写在这里一份，工具和主站都从这里取路径。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 套接字路径上限。sun_path 是 108 字节，但 sockaddr_un 的容量检查留给调用方，
 * 这里只要求放得下路径字符串本身。 */
#define EMASTER_SOCKET_PATH_CAPACITY 256U

/* 未显式指定部署时读这个环境变量。台架脚本已经在用部署 ID 做变量名，
 * 环境变量是同一件事在命令行上的延续。 */
#define EMASTER_DEPLOYMENT_ENV "EMASTER_DEPLOYMENT"

/*
 * 由部署 ID 构造命令套接字路径。成功返回 true。
 * 部署 ID 为空或结果放不下时返回 false，并把 buffer 置为空串——宁可让调用方
 * 报错，也不要留下一个半截路径去连别的部署。
 */
static inline bool emaster_command_socket_path(const char *deployment_id,
                                               char *buffer,
                                               size_t capacity)
{
    int written;

    if (buffer == NULL || capacity == 0U)
    {
        return false;
    }
    buffer[0] = '\0';
    if (deployment_id == NULL || deployment_id[0] == '\0')
    {
        return false;
    }
    written = snprintf(buffer, capacity, "/tmp/emaster-%s.sock", deployment_id);
    return written > 0 && (size_t)written < capacity;
}

/*
 * CLI 共用选项解析：把 --socket <路径> / --deployment <ID> 从 argv 里摘掉，
 * 其余参数原地左移，使调用方原有的位置参数解析完全不用改（返回新的 argc）。
 *
 * 路径优先级：--socket 显式路径 > --deployment 显式部署 > 环境变量 EMASTER_DEPLOYMENT。
 * 三者都没有时返回 false——**不去猜**。此前写死一个默认值正是这些工具静默连错
 * 主站的来源，未知时让调用方报错退出比猜一个更安全。
 *
 * 选项缺参数（--socket 后没有值）时同样返回 false，并且不消费它，调用方可以据此报错。
 */
static inline bool emaster_cli_resolve_socket(int *argc,
                                              char **argv,
                                              char *buffer,
                                              size_t capacity)
{
    const char *explicit_socket = NULL;
    const char *deployment = NULL;
    int write_index = 1;

    if (buffer == NULL || capacity == 0U)
    {
        return false;
    }
    buffer[0] = '\0';
    if (argc == NULL || argv == NULL)
    {
        return false;
    }

    for (int read_index = 1; read_index < *argc; ++read_index)
    {
        const char *argument = argv[read_index];

        if (strcmp(argument, "--socket") == 0 || strcmp(argument, "--deployment") == 0)
        {
            const bool wants_socket = strcmp(argument, "--socket") == 0;
            const char *value = (read_index + 1 < *argc) ? argv[read_index + 1] : NULL;

            if (value == NULL || value[0] == '\0' || value[0] == '-')
            {
                /* 缺参数或下一个就是另一个选项：不消费，交由调用方报错。 */
                return false;
            }
            if (wants_socket)
            {
                explicit_socket = value;
            }
            else
            {
                deployment = value;
            }
            ++read_index;
            continue;
        }
        argv[write_index++] = argv[read_index];
    }
    *argc = write_index;

    if (explicit_socket != NULL)
    {
        size_t length = strlen(explicit_socket);

        if (length + 1U > capacity)
        {
            return false;
        }
        memcpy(buffer, explicit_socket, length + 1U);
        return true;
    }
    if (deployment == NULL)
    {
        deployment = getenv(EMASTER_DEPLOYMENT_ENV);
    }
    return emaster_command_socket_path(deployment, buffer, capacity);
}

#endif /* EMASTER_BUS_COMMAND_SOCKET_PATH_H */
