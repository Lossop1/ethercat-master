/*
 * 简单的外部控制器模拟器：通过命令服务器发送位置目标
 *
 * 用法: set_target_client <pos1> [pos2] [pos3] ... [--deployment <部署ID> | --socket <路径>]
 * 例如: set_target_client --deployment orangepi-bench-dual 1000 2000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "emaster/bus/command_socket_path.h"

static int connect_to_master(const char *socket_path)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int send_target_command(int fd, int argc, char **argv)
{
    char command[512];
    char response[512];
    int i;
    int offset = 0;
    ssize_t n;

    /* 构造命令：set_external_target pos1 pos2 ... */
    offset = snprintf(command, sizeof(command), "set_external_target");

    for (i = 0; i < argc; i++) {
        offset += snprintf(command + offset, sizeof(command) - offset, " %s", argv[i]);
        if (offset >= (int)sizeof(command) - 1) {
            fprintf(stderr, "命令过长\n");
            return -1;
        }
    }
    offset += snprintf(command + offset, sizeof(command) - offset, "\n");

    /* 发送命令 */
    if (write(fd, command, offset) != offset) {
        perror("write");
        return -1;
    }

    /* 读取响应 */
    n = read(fd, response, sizeof(response) - 1);
    if (n < 0) {
        perror("read");
        return -1;
    }
    response[n] = '\0';

    printf("响应: %s", response);
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    char socket_path[EMASTER_SOCKET_PATH_CAPACITY];

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        fprintf(stderr, "用法: %s <pos1> [pos2] ... [选项]\n", argv[0]);
        fprintf(stderr, "例如: %s --deployment orangepi-bench-dual 1000 2000\n\n", argv[0]);
        fprintf(stderr, "选项:\n");
        fprintf(stderr, "  --deployment <部署ID>  连接该部署的命令套接字（默认取 $%s）\n",
                EMASTER_DEPLOYMENT_ENV);
        fprintf(stderr, "  --socket <路径>        直接指定套接字路径，优先于 --deployment\n");
        return 0;
    }
    /* 先摘掉 --socket / --deployment，剩下的才是各轴目标位置。 */
    if (!emaster_cli_resolve_socket(&argc, argv, socket_path, sizeof(socket_path))) {
        fprintf(stderr, "错误：无法确定命令套接字路径\n");
        fprintf(stderr, "  加 --deployment <部署ID>，或 --socket <路径>，"
                        "或设环境变量 %s\n", EMASTER_DEPLOYMENT_ENV);
        return 2;
    }

    if (argc < 2) {
        fprintf(stderr, "用法: %s <pos1> [pos2] ... [选项]\n", argv[0]);
        fprintf(stderr, "例如: %s --deployment orangepi-bench-dual 1000 2000\n", argv[0]);
        return 1;
    }

    fd = connect_to_master(socket_path);
    if (fd < 0) {
        fprintf(stderr, "无法连接到主站：%s\n", socket_path);
        return 1;
    }

    if (send_target_command(fd, argc - 1, argv + 1) < 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
