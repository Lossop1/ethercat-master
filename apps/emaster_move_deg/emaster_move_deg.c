/*
 * 角度控制入口：用角度代替脉冲，对用户友好
 *
 * 用法：sudo emaster-move-deg [--deployment <部署ID> | --socket <路径>]
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

#include "emaster/bus/command_socket_path.h"
#include "emaster/cli.h"

/* 套接字路径由 --socket / --deployment / $EMASTER_DEPLOYMENT 决定，见
 * command_socket_path.h。这里不再写死某个部署，main 解析失败就直接退出。 */
static char g_socket_path[EMASTER_SOCKET_PATH_CAPACITY];
#define RESPONSE_MAX 2048
#define MAX_AXES 16

typedef struct {
    unsigned int bus_position;
    unsigned int encoder_increments;
    unsigned int encoder_motor_revolutions;
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
        unsigned long long load_resolution;

        token = strtok_r(NULL, "|", &saveptr);
        if (token == NULL)
        {
            fprintf(stderr, "轴 %zu 数据缺失\n", axis_index);
            return -1;
        }

        if (sscanf(token, "a%u:bus=%u,enc=%u/%u,gear=%u/%u,torque=%u",
                   &axis_num,
                   &axis->bus_position,
                   &axis->encoder_increments,
                   &axis->encoder_motor_revolutions,
                   &axis->gear_ratio_num,
                   &axis->gear_ratio_den,
                   &torque) != 7)
        {
            fprintf(stderr, "解析轴 %zu 参数失败: %s\n", axis_index, token);
            return -1;
        }

        /*
         * 换算系数 = 电机每转计数 × 齿轮比 ÷ 360。
         *
         * 电机每转计数 = encoder_increments / encoder_motor_revolutions，两段都得用：
         * 编码器不直接装在电机轴上时（编码器每转对应电机多转），只用分子会把系数
         * 算大若干倍。乘法先做完再除，避免整数除法先截断。
         */
        if (axis->encoder_motor_revolutions == 0 || axis->gear_ratio_den == 0)
        {
            fprintf(stderr, "轴 %zu 的分辨率参数不合法（分母为 0）\n", axis_index);
            return -1;
        }
        load_resolution = (unsigned long long)axis->encoder_increments *
                          axis->gear_ratio_num /
                          ((unsigned long long)axis->encoder_motor_revolutions *
                           axis->gear_ratio_den);
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
        printf("  轴 %zu: 1° = %.2f 脉冲 (编码器 %u/%u, 齿轮比 %u:%u)\n",
               i + 1,
               axis->pulses_per_degree,
               axis->encoder_increments,
               axis->encoder_motor_revolutions,
               axis->gear_ratio_num,
               axis->gear_ratio_den);
    }

    printf("\n单位: 角度 (°)\n");
    printf("输入格式: 轴1 轴2 ... (空格分隔)\n");
    printf("示例: 45.5 120.0  (轴1移动到45.5°, 轴2移动到120.0°)\n");
    printf("输入 q 或 quit 退出\n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n\n");
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "用法: sudo %s\n\n", prog);
    fprintf(out, "说明:\n");
    fprintf(out, "  - 启动后持续运行，可反复输入各轴目标角度\n");
    fprintf(out, "  - 角度单位: 度 (°)，换算系数由主站拓扑自动查询\n");
    fprintf(out, "  - 建议配合 'sudo emaster-watch' 实时监控运动过程\n");
    fprintf(out, "  - 输入 q 或 quit 退出\n");
    fprintf(out, "\n选项:\n");
    fprintf(out, "  --deployment <部署ID>  连接该部署的命令套接字（默认取 $%s）\n",
            EMASTER_DEPLOYMENT_ENV);
    fprintf(out, "  --socket <路径>        直接指定套接字路径，优先于 --deployment\n");
    fprintf(out, "  --help: 打印本说明后退出\n");
    fprintf(out, "  --version: 打印版本号后退出\n");
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

    /*
     * 无参数会去连默认部署的命令套接字，是一个默认流程，所以不把无参数改成出帮助。
     */
    switch (emaster_cli_info(argc, argv))
    {
    case EMASTER_CLI_HELP:
        print_usage(stdout, argv[0]);
        return 0;
    case EMASTER_CLI_VERSION:
        emaster_cli_print_version("emaster-move-deg");
        return 0;
    default:
        break;
    }
    if (!emaster_cli_resolve_socket(&argc, argv, g_socket_path, sizeof(g_socket_path)))
    {
        fprintf(stderr, "错误：无法确定命令套接字路径\n");
        fprintf(stderr, "  加 --deployment <部署ID>，或 --socket <路径>，"
                        "或设环境变量 %s\n", EMASTER_DEPLOYMENT_ENV);
        return 2;
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

    if (strlen(g_socket_path) >= sizeof(addr.sun_path))
    {
        fprintf(stderr, "错误：套接字路径过长：%s\n", g_socket_path);
        close(sock_fd);
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1U);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "无法连接到主站 %s\n", g_socket_path);
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

        /* 解析角度输入：逐个取数，取满轴数即止。不写死个数，轴数变了不用改代码。 */
        {
            const char *cursor = input_line;
            size_t parsed = 0U;

            while (parsed < topo.axis_count)
            {
                char *end = NULL;
                double value = strtod(cursor, &end);

                if (end == cursor)
                {
                    break;
                }
                degrees[parsed++] = value;
                cursor = end;
            }
            if (parsed < topo.axis_count)
            {
                fprintf(stderr, "输入错误：需要 %zu 个角度值\n", topo.axis_count);
                continue;
            }
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
