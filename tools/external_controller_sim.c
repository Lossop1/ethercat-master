#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <errno.h>

static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <socket-path> [--duration-seconds N] [--frequency-hz F]\n", argv[0]);
        fprintf(stderr, "示例: %s /tmp/emaster-orangepi-current-bench.sock --duration-seconds 3600 --frequency-hz 10\n", argv[0]);
        return 1;
    }

    const char *socket_path = argv[1];
    int duration_seconds = 3600;  // 默认1小时
    double frequency_hz = 10.0;   // 默认10Hz发送频率

    // 解析可选参数
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--duration-seconds") == 0 && i + 1 < argc) {
            duration_seconds = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--frequency-hz") == 0 && i + 1 < argc) {
            frequency_hz = atof(argv[i + 1]);
            i++;
        }
    }

    const long sleep_us = (long)(1000000.0 / frequency_hz);

    printf("外部控制器模拟器启动\n");
    printf("  Socket: %s\n", socket_path);
    printf("  持续时间: %d 秒 (%.1f 分钟)\n", duration_seconds, duration_seconds / 60.0);
    printf("  发送频率: %.1f Hz (每 %ld 微秒)\n", frequency_hz, sleep_us);
    printf("  运动模式: 正弦波位置指令 (±180°)\n");
    printf("\n按 Ctrl+C 提前停止\n\n");

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 连接到主站命令服务器
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    printf("正在连接到主站...\n");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    printf("✓ 已连接\n\n");

    // 开始发送位置指令
    time_t start_time = time(NULL);
    time_t end_time = start_time + duration_seconds;
    unsigned long command_count = 0;
    unsigned long error_count = 0;

    printf("开始发送位置指令...\n");
    printf("时间(s) | 指令数 | 错误数 | 当前目标(°)\n");
    printf("--------|--------|--------|-------------\n");

    time_t last_report = start_time;

    while (running && time(NULL) < end_time) {
        // 生成正弦波位置目标：±180°，周期 10 秒
        double elapsed = difftime(time(NULL), start_time);
        double angle_deg = 180.0 * sin(2.0 * M_PI * elapsed / 10.0);

        // 将角度转换为编码器 counts (16384 counts/rev)
        long target_counts = (long)(angle_deg * 16384.0 / 360.0);

        // 构造命令：target <axis-id> <position>
        char command[256];
        snprintf(command, sizeof(command), "target bench_axis_01 %ld\n", target_counts);

        // 发送命令
        ssize_t sent = send(sock, command, strlen(command), 0);
        if (sent < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                fprintf(stderr, "\n错误：主站连接断开\n");
                break;
            }
            error_count++;
        } else {
            command_count++;
        }

        // 每秒报告一次
        time_t now = time(NULL);
        if (now > last_report) {
            double elapsed_total = difftime(now, start_time);
            printf("\r%7.0f | %6lu | %6lu | %+8.1f°",
                   elapsed_total, command_count, error_count, angle_deg);
            fflush(stdout);
            last_report = now;
        }

        usleep(sleep_us);
    }

    printf("\n\n");

    // 统计
    time_t actual_duration = time(NULL) - start_time;
    printf("测试完成\n");
    printf("  实际运行时间: %ld 秒 (%.1f 分钟)\n", actual_duration, actual_duration / 60.0);
    printf("  发送指令总数: %lu\n", command_count);
    printf("  错误次数: %lu\n", error_count);
    printf("  平均发送速率: %.1f 命令/秒\n", (double)command_count / actual_duration);

    if (error_count > 0) {
        printf("\n⚠ 检测到 %lu 次发送错误\n", error_count);
    } else {
        printf("\n✓ 所有指令发送成功\n");
    }

    close(sock);
    return error_count > 0 ? 1 : 0;
}
