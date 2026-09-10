/*
 * 人工监控入口：常驻显示主站实时状态
 *
 * 用法：sudo emaster-watch [刷新间隔ms，默认200]
 *
 * 存在原因：操作者需要一个简单命令就能启动实时监控，
 * 不需要记忆socket路径、参数格式等细节。
 *
 * 动态拓扑：启动时查询主站实际轴数和参数，支持任意轴数配置。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define SOCKET_PATH "/tmp/emaster-orangepi-bench-dual.sock"
#define DEFAULT_REFRESH_MS 200
#define RESPONSE_MAX 2048
#define MAX_AXES 16

typedef struct {
    unsigned int bus_position;
    unsigned int encoder_resolution;  /* 编码器分辨率（脉冲/转） */
    unsigned int gear_ratio_num;      /* 齿轮比分子 */
    unsigned int gear_ratio_den;      /* 齿轮比分母 */
    unsigned int rated_torque_mnm;    /* 额定力矩（毫牛米） */
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

static void print_header(void)
{
    printf("\033[H\033[J");  /* 清屏并回到顶部 */
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf("  EtherCAT 主站实时监控 — 按 Ctrl+C 退出\n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "用法: sudo %s [刷新间隔ms]\n", prog);
    fprintf(stderr, "示例: sudo %s 200\n\n", prog);
    fprintf(stderr, "说明:\n");
    fprintf(stderr, "  - 位置单位: 脉冲 (pulse)\n");
    fprintf(stderr, "  - 速度单位: 脉冲/秒 (pulse/s)\n");
    fprintf(stderr, "  - 力矩单位: 千分之额定力矩 (‰)\n");
    fprintf(stderr, "  - 跟随误差 = 实际位置 - PDO目标，反映驱动器响应情况\n");
    fprintf(stderr, "  - 状态字 0x1237 = Operation Enabled (正常使能)\n");
    fprintf(stderr, "  - 必须用 sudo 运行（socket 属主为 root）\n");
}

/* 查询拓扑：解析 "axes=N|a1:bus=B,enc=E,gear=G1/G2,torque=T|..." */
static int query_topology(int sock_fd, topology_t *topo)
{
    char request[] = "topology\n";
    char response[RESPONSE_MAX];
    ssize_t sent, received;
    char *token, *saveptr;
    size_t axis_index;

    /* 发送查询 */
    sent = write(sock_fd, request, sizeof(request) - 1);
    if (sent <= 0)
    {
        fprintf(stderr, "发送拓扑查询失败\n");
        return -1;
    }

    /* 读取响应 */
    received = read(sock_fd, response, sizeof(response) - 1);
    if (received <= 0)
    {
        fprintf(stderr, "读取拓扑响应失败\n");
        return -1;
    }
    response[received] = '\0';

    /* 解析响应：OK|axes=N|a1:...|a2:... */
    if (strncmp(response, "OK|", 3) != 0)
    {
        fprintf(stderr, "拓扑查询失败: %s\n", response);
        return -1;
    }

    /* 跳过 "OK|" */
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

    /* 解析各轴参数 */
    for (axis_index = 0; axis_index < topo->axis_count; axis_index++)
    {
        axis_config_t *axis = &topo->axes[axis_index];
        unsigned int axis_num;

        token = strtok_r(NULL, "|", &saveptr);
        if (token == NULL)
        {
            fprintf(stderr, "轴 %zu 数据缺失\n", axis_index);
            return -1;
        }

        /* 解析 "a1:bus=B,enc=E,gear=G1/G2,torque=T" */
        if (sscanf(token, "a%u:bus=%u,enc=%u,gear=%u/%u,torque=%u",
                   &axis_num,
                   &axis->bus_position,
                   &axis->encoder_resolution,
                   &axis->gear_ratio_num,
                   &axis->gear_ratio_den,
                   &axis->rated_torque_mnm) != 6)
        {
            fprintf(stderr, "解析轴 %zu 参数失败: %s\n", axis_index, token);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int sock_fd;
    struct sockaddr_un addr;
    long refresh_ms = DEFAULT_REFRESH_MS;
    struct timespec sleep_time;
    char request[] = "status\n";
    char response[RESPONSE_MAX];
    struct sigaction sa;
    topology_t topo;
    size_t i;

    if (argc > 1)
    {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        refresh_ms = strtol(argv[1], NULL, 10);
        if (refresh_ms < 50 || refresh_ms > 5000)
        {
            fprintf(stderr, "刷新间隔应在 50-5000ms 之间\n");
            return 1;
        }
    }

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

    /* 显示发现的拓扑 */
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf("  发现 %zu 轴系统\n", topo.axis_count);
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");
    for (i = 0; i < topo.axis_count; i++)
    {
        const axis_config_t *axis = &topo.axes[i];
        unsigned long load_resolution = (unsigned long)axis->encoder_resolution * axis->gear_ratio_num / axis->gear_ratio_den;

        printf("轴 %zu:\n", i + 1);
        printf("  总线位置: %u\n", axis->bus_position);
        printf("  编码器分辨率: %u 脉冲/转（电机侧）\n", axis->encoder_resolution);
        printf("  齿轮比: %u:%u\n", axis->gear_ratio_num, axis->gear_ratio_den);
        printf("  负载侧分辨率: %lu 脉冲/转\n", load_resolution);
        printf("  角度换算: 1° = %lu 脉冲\n", load_resolution / 360);
        printf("  额定力矩: %u mNm\n", axis->rated_torque_mnm);
        printf("\n");
    }
    printf("按 Ctrl+C 退出...\n");
    sleep(2);

    sleep_time.tv_sec = refresh_ms / 1000;
    sleep_time.tv_nsec = (refresh_ms % 1000) * 1000000L;

    /* 主循环：持续查询并原地刷新 */
    while (running)
    {
        ssize_t sent, received;

        /* 发送查询 */
        sent = write(sock_fd, request, sizeof(request) - 1);
        if (sent <= 0)
        {
            fprintf(stderr, "\n连接断开\n");
            break;
        }

        /* 读取响应 */
        received = read(sock_fd, response, sizeof(response) - 1);
        if (received <= 0)
        {
            fprintf(stderr, "\n读取失败\n");
            break;
        }
        response[received] = '\0';

        /* 显示 */
        print_header();
        if (strncmp(response, "OK|", 3) == 0)
        {
            /* 去掉 OK| 前缀，直接显示主站输出 */
            printf("%s\n", response + 3);
        }
        else
        {
            printf("主站响应: %s\n", response);
        }

        printf("\n─────────────────────────────────────────────────────────────────────────────\n");
        printf("单位说明: 位置=脉冲 | 速度=脉冲/秒 | 力矩=‰额定 | 刷新=%ldms\n", refresh_ms);
        fflush(stdout);

        nanosleep(&sleep_time, NULL);
    }

    printf("\n\n监控已停止\n");
    close(sock_fd);
    return 0;
}
