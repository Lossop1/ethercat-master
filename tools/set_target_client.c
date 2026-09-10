/*
 * 简单的外部控制器模拟器：通过命令服务器发送位置目标
 *
 * 用法: set_target_client <socket_path> <pos1> [pos2] [pos3] ...
 * 例如: set_target_client /tmp/emaster-orangepi-bench-dual.sock 1000 2000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

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
    const char *socket_path;

    if (argc < 3) {
        fprintf(stderr, "用法: %s <socket_path> <pos1> [pos2] ...\n", argv[0]);
        fprintf(stderr, "例如: %s /tmp/emaster-orangepi-bench-dual.sock 1000 2000\n", argv[0]);
        return 1;
    }

    socket_path = argv[1];

    fd = connect_to_master(socket_path);
    if (fd < 0) {
        fprintf(stderr, "无法连接到主站：%s\n", socket_path);
        return 1;
    }

    if (send_target_command(fd, argc - 2, argv + 2) < 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
