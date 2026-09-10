/*
 * 人工控制入口：发送位置目标到主站
 *
 * 用法：sudo emaster-move <轴1位置> <轴2位置> [...]
 *   或：sudo emaster-move（交互式输入）
 *
 * 存在原因：操作者需要一个简单命令就能控制电机，
 * 不需要记忆socket路径、命令格式等细节。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/emaster-orangepi-bench-dual.sock"
#define MAX_AXES 16
#define COMMAND_MAX 512
#define RESPONSE_MAX 512

static void print_usage(const char *prog)
{
    fprintf(stderr, "用法: sudo %s\n\n", prog);
    fprintf(stderr, "说明:\n");
    fprintf(stderr, "  - 启动后持续运行，可反复输入目标位置\n");
    fprintf(stderr, "  - 位置单位: 脉冲 (pulse)\n");
    fprintf(stderr, "  - 范围: 根据电机编码器分辨率，通常 ±2147483647\n");
    fprintf(stderr, "  - 建议配合 'sudo emaster-watch' 实时监控运动过程\n");
    fprintf(stderr, "  - 输入 q 退出\n");
}

static int send_command(const char *command, char *response, size_t response_size)
{
    int sock_fd;
    struct sockaddr_un addr;
    ssize_t sent, received;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "无法连接到主站 %s\n", SOCKET_PATH);
        fprintf(stderr, "请确认:\n");
        fprintf(stderr, "  1. 主站是否在运行\n");
        fprintf(stderr, "  2. 是否使用了 sudo\n");
        close(sock_fd);
        return -1;
    }

    /* 发送命令 */
    sent = write(sock_fd, command, strlen(command));
    if (sent <= 0)
    {
        perror("write");
        close(sock_fd);
        return -1;
    }

    /* 读取响应 */
    received = read(sock_fd, response, response_size - 1);
    if (received <= 0)
    {
        perror("read");
        close(sock_fd);
        return -1;
    }
    response[received] = '\0';

    /* 去掉换行符 */
    char *newline = strchr(response, '\n');
    if (newline != NULL)
    {
        *newline = '\0';
    }

    close(sock_fd);
    return 0;
}

static int interactive_mode(void)
{
    char input[256];
    char command[COMMAND_MAX];
    char response[RESPONSE_MAX];
    long positions[MAX_AXES];
    int axis_count;
    int i;

    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf("  EtherCAT 主站交互式控制\n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");
    printf("当前配置: 2 轴\n");
    printf("位置单位: 脉冲 (pulse)\n");
    printf("输入格式: 轴1 轴2（空格分隔），输入 q 退出\n\n");

    while (1)
    {
        printf("位置> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            break;
        }

        /* 去掉换行 */
        char *newline = strchr(input, '\n');
        if (newline != NULL)
        {
            *newline = '\0';
        }

        /* 检查退出 */
        if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0)
        {
            printf("退出\n");
            break;
        }

        /* 解析输入 */
        axis_count = sscanf(input, "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
                            &positions[0], &positions[1], &positions[2], &positions[3],
                            &positions[4], &positions[5], &positions[6], &positions[7],
                            &positions[8], &positions[9], &positions[10], &positions[11],
                            &positions[12], &positions[13], &positions[14], &positions[15]);

        if (axis_count < 1)
        {
            fprintf(stderr, "输入无效，请输入至少一个数字\n");
            continue;
        }

        /* 构造命令 */
        int offset = snprintf(command, sizeof(command), "set_external_target");
        for (i = 0; i < axis_count; i++)
        {
            offset += snprintf(command + offset, sizeof(command) - offset, " %ld", positions[i]);
        }
        offset += snprintf(command + offset, sizeof(command) - offset, "\n");

        /* 发送 */
        if (send_command(command, response, sizeof(response)) < 0)
        {
            fprintf(stderr, "发送失败\n");
            continue;
        }

        if (strncmp(response, "OK|", 3) == 0)
        {
            printf("-> 已发送: ");
            for (i = 0; i < axis_count; i++)
            {
                printf("轴%d=%ld ", i + 1, positions[i]);
            }
            printf("\n");
        }
        else
        {
            printf("-> 失败: %s\n", response);
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(argv[0]);
        return 0;
    }

    /* 始终进入交互式模式 */
    return interactive_mode();
}
