/*
 * 角度控制入口：用角度代替脉冲，对用户友好
 *
 * 用法：sudo emaster-move-deg
 *
 * 存在原因：直接输入脉冲（如 458752）对人类不友好，
 * 角度（如 45.5°）更直观。工具启动时查询编码器参数，
 * 自动计算换算系数。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/emaster-orangepi-bench-dual.sock"
#define RESPONSE_MAX 2048
#define MAX_AXES 16

typedef struct {
    unsigned int bus_position;
    unsigned int encoder_resolution;
    unsigned int gear_ratio_num;
    unsigned int gear_ratio_den;
    double pulses_per_degree;  /* 1° = ? 脉冲 */
} axis_config_t;

typedef struct {
    size_t axis_count;
    axis_config_t axes[MAX_AXES];
} topology_t;

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* 查询拓扑并计算换算系数 */
static int query_topology(int sock_fd, topology_t *topo)
{
    char request[] = "topology\n";
    char response[RESPONSE_MAX];
    ssize_t sent, received;
    char *token, *saveptr;
    size_t axis_index;

    sent = write(sock_fd, request, sizeof(request) - 1);
    if (sent <= 0)
    {
        fprintf(stderr, "发送拓扑查询失败\n");
        return -1;
    }

    received = read(sock_fd, response, sizeof(response) - 1);
    if (received <= 0)
    {
        fprintf(stderr, "读取拓扑响应失败\n");
        return -1;
    }
    response[received] = '\0';

    if (strncmp(response, "OK|", 3) != 0)
    {
        fprintf(stderr, "拓扑查询失败: %s\n", response);
        return -1;
    }

    token = strtok_r(response + 3, "|", &saveptr);
    if (token == NULL || sscanf(token, "axes=%zu", &topo->axis_count) != 1)
    {
        fprintf(stderr, "解析轴数失败\n");
        return -1;
    }

    if (topo->axis_count == 0 || topo->axis_count > MAX_AXES)
    {
        fprintf(stderr, "轴数无效: %zu\n", topo->axis_count);
        return -1;
    }

    for (axis_index = 0; axis_index < topo->axis_count; axis_index++)
    {
        axis_config_t *axis = &topo->axes[axis_index];
        unsigned int axis_num, torque;
        unsigned long load_resolution;

        token = strtok_r(NULL, "|", &saveptr);
        if (token == NULL)
        {
            fprintf(stderr, "轴 %zu 数据缺失\n", axis_index);
            return -1;
        }

        if (sscanf(token, "a%u:bus=%u,enc=%u,gear=%u/%u,torque=%u",
                   &axis_num,
                   &axis->bus_position,
                   &axis->encoder_resolution,
                   &axis->gear_ratio_num,
                   &axis->gear_ratio_den,
                   &torque) != 6)
        {
            fprintf(stderr, "解析轴 %zu 参数失败: %s\n", axis_index, token);
            return -1;
        }

        /* 计算换算系数：负载侧分辨率 / 360 */
        load_resolution = (unsigned long)axis->encoder_resolution * axis->gear_ratio_num / axis->gear_ratio_den;
        axis->pulses_per_degree = (double)load_resolution / 360.0;
    }

    return 0;
}

static void print_header(const topology_t *topo)
{
    size_t i;

    printf("\033[2J\033[H");  /* 清屏 */
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf("  EtherCAT 主站角度控制\n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");

    printf("发现 %zu 轴系统，换算系数：\n", topo->axis_count);
    for (i = 0; i < topo->axis_count; i++)
    {
        const axis_config_t *axis = &topo->axes[i];
        printf("  轴 %zu: 1° = %.2f 脉冲 (编码器 %u, 齿轮比 %u:%u)\n",
               i + 1,
               axis->pulses_per_degree,
               axis->encoder_resolution,
               axis->gear_ratio_num,
               axis->gear_ratio_den);
    }

    printf("\n单位: 角度 (°)\n");
    printf("输入格式: 轴1 轴2 ... (空格分隔)\n");
    printf("示例: 45.5 120.0  (轴1移动到45.5°, 轴2移动到120.0°)\n");
    printf("输入 q 或 quit 退出\n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");
}

int main(int argc, char **argv)
{
    int sock_fd;
    struct sockaddr_un addr;
    topology_t topo;
    char input_line[256];
    char command[512];
    char response[RESPONSE_MAX];
    struct sigaction sa;

    (void)argc;
    (void)argv;

    /* 安装信号处理 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
    {
        perror("sigaction");
        return 1;
    }

    /* 连接主站 */
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        return 1;
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
        return 1;
    }

    /* 查询拓扑 */
    if (query_topology(sock_fd, &topo) < 0)
    {
        close(sock_fd);
        return 1;
    }

    print_header(&topo);

    /* 主循环 */
    while (running)
    {
        double degrees[MAX_AXES];
        int32_t pulses[MAX_AXES];
        size_t i;
        ssize_t sent, received;
        int offset;

        printf("角度> ");
        fflush(stdout);

        if (fgets(input_line, sizeof(input_line), stdin) == NULL)
        {
            break;
        }

        /* 去除换行符 */
        input_line[strcspn(input_line, "\n")] = '\0';

        /* 检查退出命令 */
        if (strcmp(input_line, "q") == 0 || strcmp(input_line, "quit") == 0)
        {
            break;
        }

        /* 解析角度输入 */
        if (sscanf(input_line, "%lf %lf %lf %lf %lf %lf %lf %lf",
                   &degrees[0], &degrees[1], &degrees[2], &degrees[3],
                   &degrees[4], &degrees[5], &degrees[6], &degrees[7]) < (int)topo.axis_count)
        {
            fprintf(stderr, "输入错误：需要 %zu 个角度值\n", topo.axis_count);
            continue;
        }

        /* 转换为脉冲 */
        for (i = 0; i < topo.axis_count; i++)
        {
            pulses[i] = (int32_t)(degrees[i] * topo.axes[i].pulses_per_degree);
        }

        /* 构造命令 */
        offset = snprintf(command, sizeof(command), "set_external_target");
        for (i = 0; i < topo.axis_count; i++)
        {
            offset += snprintf(command + offset, sizeof(command) - offset, " %d", pulses[i]);
        }
        offset += snprintf(command + offset, sizeof(command) - offset, "\n");

        /* 发送命令 */
        sent = write(sock_fd, command, offset);
        if (sent <= 0)
        {
            fprintf(stderr, "发送命令失败\n");
            break;
        }

        /* 读取响应 */
        received = read(sock_fd, response, sizeof(response) - 1);
        if (received <= 0)
        {
            fprintf(stderr, "读取响应失败\n");
            break;
        }
        response[received] = '\0';

        /* 显示结果 */
        if (strncmp(response, "OK", 2) == 0)
        {
            printf("✓ 目标已设置: ");
            for (i = 0; i < topo.axis_count; i++)
            {
                printf("轴%zu=%.2f° (%d脉冲)%s",
                       i + 1,
                       degrees[i],
                       pulses[i],
                       (i < topo.axis_count - 1) ? ", " : "\n");
            }
        }
        else
        {
            printf("✗ 命令失败: %s\n", response);
        }
        printf("\n");
    }

    printf("\n角度控制已退出\n");
    close(sock_fd);
    return 0;
}
