#define _POSIX_C_SOURCE 200809L

/*
 * 人工监控客户端：通过命令服务器读取主站周期状态。
 *
 * 存在原因：主站此前只能「写」（set_external_target），操作者看不到任何回读，
 * 只能靠事后报告事后判断。本工具补上读取方向，使「发目标 -> 看实际位置/跟随误差」
 * 的闭环可以在终端里直接完成。
 *
 * 单次模式打印一份可读状态；--watch 模式周期刷新，用 \r 原地重绘不滚动屏幕。
 * 数据来源是主站命令响应，不直连网卡，也不修改任何运行参数。
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "emaster/bus/command_socket_path.h"
#include "emaster/cli.h"
#include "emaster/cli/option_match.h"

#define STATUS_RESPONSE_MAX 2048
#define STATUS_MAX_AXES 16

/* 一轴的解析结果。字段缺失时 parsed 为 false，显示为 "-" 而不是 0。 */
typedef struct
{
    int index;
    bool parsed;
    long position;
    long velocity;
    long torque;
    unsigned long status_word;
    long target_position;
    long planned_position;
    unsigned long error_code;
    int cia_state;
} status_axis_t;

typedef struct
{
    bool parsed;
    long state;
    long cycle;
    long axis_count;
    int enabled;
    int completed;
    /*
     * 主站加载的运行时配置指纹。valid 为假时显示 na：0 是合法哈希值，不能兼作
     * "没加载"。parse_summary_field 只在 cfg 字段真的出现且不是 na 时置真。
     */
    bool fingerprint_valid;
    unsigned long long fingerprint;
    bool truncated;
    status_axis_t axes[STATUS_MAX_AXES];
    size_t axis_seen;
} status_frame_t;

static volatile sig_atomic_t watch_stop = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    watch_stop = 1;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0)
    {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

static int connect_to_master(const char *socket_path)
{
    int fd;
    struct sockaddr_un addr;

    if (strlen(socket_path) >= sizeof(addr.sun_path))
    {
        fprintf(stderr, "错误：套接字路径过长：%s\n", socket_path);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* 长度上面已经卡过，memcpy 不会越界；addr 已清零，末尾留着 NUL。 */
    memcpy(addr.sun_path, socket_path, strlen(socket_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * 读取一条完整响应。命令服务器对每条命令写一行，因此按换行符判定结束，
 * 避免把响应切成两段后解析出半个字段。
 */
static ssize_t read_response(int fd, char *buffer, size_t capacity)
{
    size_t used = 0U;

    while (used + 1U < capacity)
    {
        ssize_t got = read(fd, buffer + used, capacity - 1U - used);

        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (got == 0)
        {
            break;
        }
        used += (size_t)got;
        buffer[used] = '\0';
        if (memchr(buffer, '\n', used) != NULL)
        {
            break;
        }
    }
    return (ssize_t)used;
}

/* 命令服务器统一以 "OK|" 或 "ERROR|" 开头，这里只做前缀切分，不解释语义。 */
static int send_command(int fd, const char *request, char *buffer, size_t capacity)
{
    char *payload;
    char *newline;

    if (write(fd, request, strlen(request)) != (ssize_t)strlen(request))
    {
        perror("write");
        return -1;
    }
    if (read_response(fd, buffer, capacity) <= 0)
    {
        fprintf(stderr, "错误：未收到主站响应\n");
        return -1;
    }
    newline = strchr(buffer, '\n');
    if (newline != NULL)
    {
        *newline = '\0';
    }
    if (strncmp(buffer, "OK|", 3) != 0)
    {
        fprintf(stderr, "主站返回错误：%s\n", buffer);
        return -1;
    }
    payload = buffer + 3;
    /* read_response 是原地解析，调用者拿到的是去掉前缀后的同一缓冲区。 */
    memmove(buffer, payload, strlen(payload) + 1U);
    return 0;
}

/* 顶层字段：段内以空格分隔的若干 key=value，逐个识别，未知键跳过。 */
static void parse_summary_field(char *field, status_frame_t *frame)
{
    char *equals = strchr(field, '=');
    char *value;

    if (equals == NULL)
    {
        return;
    }
    *equals = '\0';
    value = equals + 1;
    if (strcmp(field, "state") == 0)
    {
        frame->state = strtol(value, NULL, 10);
    }
    else if (strcmp(field, "cycle") == 0)
    {
        frame->cycle = strtol(value, NULL, 10);
    }
    else if (strcmp(field, "axes") == 0)
    {
        frame->axis_count = strtol(value, NULL, 10);
        frame->parsed = true;
    }
    else if (strcmp(field, "enabled") == 0)
    {
        frame->enabled = (int)strtol(value, NULL, 10);
    }
    else if (strcmp(field, "completed") == 0)
    {
        frame->completed = (int)strtol(value, NULL, 10);
    }
    else if (strcmp(field, "cfg") == 0)
    {
        /* "na" 表示主站没加载过配置；按 0 解析会把 na 读成合法指纹 0。 */
        if (strcmp(value, "na") != 0)
        {
            frame->fingerprint = strtoull(value, NULL, 16);
            frame->fingerprint_valid = true;
        }
    }
}

/*
 * 解析 "key=value" 形式的状态行。
 * 顶层字段集中在第一个 '|' 段内、以空格分隔；轴字段以 '|' 分段、段内以 ',' 分隔。
 * 未知键直接跳过，这样主站将来增加字段不会让旧客户端解析失败。
 */
static void parse_frame(const char *text, status_frame_t *frame)
{
    const char *cursor = text;

    memset(frame, 0, sizeof(*frame));
    frame->enabled = -1;
    frame->completed = -1;
    while (*cursor != '\0')
    {
        const char *segment_end = strchr(cursor, '|');
        size_t segment_length = segment_end == NULL ? strlen(cursor)
                                                    : (size_t)(segment_end - cursor);

        /*
         * 轴段的判别必须包含 ':'，否则形如 "state=4" 的普通字段会被当成 a4 轴，
         * 静默吃掉顶层 status 字段。
         */
        if (segment_length > 1U && cursor[0] == 'a' && cursor[1] >= '0' && cursor[1] <= '9' &&
            segment_length > 2U && cursor[2] == ':')
        {
            char segment[512];
            char *field;
            char *saveptr = NULL;
            int index = atoi(cursor + 1);
            status_axis_t *axis = NULL;

            if (segment_length >= sizeof(segment))
            {
                segment_length = sizeof(segment) - 1U;
            }
            memcpy(segment, cursor, segment_length);
            segment[segment_length] = '\0';
            if (index >= 1 && index <= STATUS_MAX_AXES)
            {
                axis = &frame->axes[index - 1];
                axis->index = index;
                axis->parsed = true;
                if (frame->axis_seen < (size_t)index)
                {
                    frame->axis_seen = (size_t)index;
                }
            }
            /*
             * 跳过 "aN:" 前缀：cursor+2 指向冒号，字段实际从冒号后一个字符开始。
             * 少跳这一个字符会让首个字段变成 ":pos"，静默丢掉轴的实际位置。
             */
            for (field = strtok_r(segment + 3, ",", &saveptr); field != NULL;
                 field = strtok_r(NULL, ",", &saveptr))
            {
                char *equals = strchr(field, '=');

                if (equals == NULL || axis == NULL)
                {
                    continue;
                }
                *equals = '\0';
                if (strcmp(field, "pos") == 0)
                {
                    axis->position = strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "vel") == 0)
                {
                    axis->velocity = strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "torque") == 0)
                {
                    axis->torque = strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "target_pos") == 0)
                {
                    axis->target_position = strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "planned") == 0)
                {
                    axis->planned_position = strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "state") == 0)
                {
                    axis->cia_state = (int)strtol(equals + 1, NULL, 10);
                }
                else if (strcmp(field, "status") == 0)
                {
                    axis->status_word = strtoul(equals + 1, NULL, 16);
                }
                else if (strcmp(field, "err") == 0)
                {
                    axis->error_code = strtoul(equals + 1, NULL, 16);
                }
            }
        }
        else
        {
            char segment[512];
            char *field;
            char *saveptr = NULL;

            if (segment_length >= sizeof(segment))
            {
                segment_length = sizeof(segment) - 1U;
            }
            memcpy(segment, cursor, segment_length);
            segment[segment_length] = '\0';
            for (field = strtok_r(segment, " ", &saveptr); field != NULL;
                 field = strtok_r(NULL, " ", &saveptr))
            {
                parse_summary_field(field, frame);
            }
        }
        if (segment_end == NULL)
        {
            break;
        }
        cursor = segment_end + 1;
    }
}

/*
 * 跟随误差是操作者判断「驱动器到底动没动」的唯一直接量。
 * 主站不单独回读，这里用 PDO 目标与实际位置的差值在客户端计算，含义与主站一致。
 */
static long following_error(const status_axis_t *axis)
{
    return axis->position - axis->target_position;
}

static void print_frame(const status_frame_t *frame)
{
    size_t index;
    char fingerprint_text[24];

    if (frame->fingerprint_valid)
    {
        (void)snprintf(fingerprint_text, sizeof(fingerprint_text), "0x%016llX",
                       frame->fingerprint);
    }
    else
    {
        (void)snprintf(fingerprint_text, sizeof(fingerprint_text), "na");
    }
    printf("state=%ld cycle=%ld axes=%ld enabled=%s completed=%s cfg=%s\n",
           frame->state, frame->cycle, frame->axis_count,
           frame->enabled < 0 ? "-" : (frame->enabled ? "是" : "否"),
           frame->completed < 0 ? "-" : (frame->completed ? "是" : "否"),
           fingerprint_text);
    if (frame->axis_seen == 0U)
    {
        printf("（未解析到轴数据，主站可能尚未进入使能状态）\n");
        return;
    }
    printf("%-4s %-12s %-10s %-12s %-10s %-12s %-8s %-6s %s\n",
           "轴", "实际位置", "速度", "PDO目标", "计划目标", "跟随误差", "状态字",
           "CiA402", "故障码");
    for (index = 0U; index < frame->axis_seen && index < STATUS_MAX_AXES; ++index)
    {
        const status_axis_t *axis = &frame->axes[index];

        if (!axis->parsed)
        {
            printf("%-4zu （无数据）\n", index + 1U);
            continue;
        }
        printf("%-4d %-12ld %-10ld %-12ld %-10ld %-12ld 0x%04lx %-6d 0x%04lx%s\n",
               axis->index, axis->position, axis->velocity, axis->target_position,
               axis->planned_position, following_error(axis), axis->status_word,
               axis->cia_state, axis->error_code,
               (axis->planned_position != axis->target_position) ? " 目标未下发" : "");
    }
    if (frame->truncated)
    {
        printf("（响应被截断，部分轴未显示）\n");
    }
}

static bool frame_is_truncated(const char *text)
{
    return strstr(text, "TRUNC") != NULL;
}

/*
 * AL 状态的四个位是编码，低四位是状态，0x10 是错误位。只翻译这两种，
 * 其余位（如 0x20 的从站需要重新配置）照原值打出来，不替读的人解释。
 */
static void format_al_state(const char *raw_text, char *out, size_t capacity)
{
    unsigned long raw;
    char *end = NULL;
    unsigned state;
    const char *name;

    if (strcmp(raw_text, "na") == 0)
    {
        (void)snprintf(out, capacity, "na");
        return;
    }
    raw = strtoul(raw_text, &end, 10);
    if (end == raw_text || *end != '\0' || raw > 0xFFUL)
    {
        (void)snprintf(out, capacity, "%s", raw_text);
        return;
    }
    state = (unsigned)(raw & 0x0FUL);
    switch (state)
    {
    case 1U:
        name = "INIT";
        break;
    case 2U:
        name = "PRE-OP";
        break;
    case 3U:
        name = "BOOT";
        break;
    case 4U:
        name = "SAFE-OP";
        break;
    case 8U:
        name = "OP";
        break;
    default:
        name = "?";
        break;
    }
    (void)snprintf(out, capacity, "%lu(%s%s)", raw, name,
                   (raw & 0x10UL) != 0UL ? "+错误位" : "");
}

/* `al` 的响应：首段是汇总，其余每段一台。读不到的台印 na，不印 0。 */
static void print_al_response(const char *text)
{
    const char *cursor = text;
    bool header_printed = false;

    while (*cursor != '\0')
    {
        const char *segment_end = strchr(cursor, '|');
        size_t length = segment_end == NULL ? strlen(cursor)
                                            : (size_t)(segment_end - cursor);
        char segment[256];

        if (length >= sizeof(segment))
        {
            length = sizeof(segment) - 1U;
        }
        memcpy(segment, cursor, length);
        segment[length] = '\0';

        if (length > 2U && cursor[0] == 'a' && cursor[1] >= '0' && cursor[1] <= '9' &&
            cursor[2] == ':')
        {
            char bus_text[16] = "na";
            char al_text[16] = "na";
            char code_text[16] = "na";
            char state_text[32];
            char *field;
            char *saveptr = NULL;

            for (field = strtok_r(segment + 3, ",", &saveptr); field != NULL;
                 field = strtok_r(NULL, ",", &saveptr))
            {
                char *equals = strchr(field, '=');

                if (equals == NULL)
                {
                    continue;
                }
                *equals = '\0';
                if (strcmp(field, "bus") == 0)
                {
                    (void)snprintf(bus_text, sizeof(bus_text), "%s", equals + 1);
                }
                else if (strcmp(field, "al") == 0)
                {
                    (void)snprintf(al_text, sizeof(al_text), "%s", equals + 1);
                }
                else if (strcmp(field, "code") == 0)
                {
                    (void)snprintf(code_text, sizeof(code_text), "%s", equals + 1);
                }
            }
            format_al_state(al_text, state_text, sizeof(state_text));
            if (!header_printed)
            {
                printf("%-4s %-6s %-16s %s\n", "轴", "总线", "AL", "状态码");
                header_printed = true;
            }
            printf("%-4d %-6s %-16s %s\n", atoi(segment + 1), bus_text, state_text,
                   code_text);
        }
        else
        {
            /* 汇总段是 "axes=12,ok=12"；逗号换空格即可读。 */
            char *comma;

            while ((comma = strchr(segment, ',')) != NULL)
            {
                *comma = ' ';
            }
            printf("%s\n", segment);
        }
        if (segment_end == NULL)
        {
            break;
        }
        cursor = segment_end + 1;
    }
    if (!header_printed)
    {
        printf("（没有逐轴数据）\n");
    }
}

static void print_usage(FILE *out, const char *program)
{
    fprintf(out, "用法：%s [--watch 毫秒] [选项]\n", program);
    fprintf(out, "  --deployment <部署ID>  连接该部署的命令套接字（默认取 $%s）\n",
            EMASTER_DEPLOYMENT_ENV);
    fprintf(out, "  --socket <路径>        直接指定套接字路径，优先于 --deployment\n");
    fprintf(out, "  --watch：周期刷新；不指定时只读一次\n");
    fprintf(out, "  --al：读取每台从站的 AL 状态与 AL 状态码，读一次后退出\n");
    fprintf(out, "  --help：打印本说明后退出\n");
    fprintf(out, "  --version：打印版本号后退出\n");
}

int main(int argc, char **argv)
{
    char socket_path[EMASTER_SOCKET_PATH_CAPACITY];
    long watch_ms = 0;
    bool al_mode = false;
    int fd;

    /*
     * 无参数会去读默认部署的状态，是一个默认流程，所以不把无参数改成出帮助。
     */
    switch (emaster_cli_info(argc, argv))
    {
    case EMASTER_CLI_HELP:
        print_usage(stdout, argv[0]);
        return 0;
    case EMASTER_CLI_VERSION:
        emaster_cli_print_version("emaster-status");
        return 0;
    default:
        break;
    }
    /* 先摘掉 --socket / --deployment，剩下的只可能是 --watch。 */
    if (!emaster_cli_resolve_socket(&argc, argv, socket_path, sizeof(socket_path)))
    {
        fputs("错误：无法确定命令套接字路径\n", stderr);
        fprintf(stderr, "  加 --deployment <部署ID>，或 --socket <路径>，"
                        "或设环境变量 %s\n", EMASTER_DEPLOYMENT_ENV);
        return 2;
    }
    /*
     * 认不出来的选项要在这里停下。先前不查这个，`--bogus` 被直接忽略，工具照常读一次
     * 状态就退出，打错的选项一声不吭。`--watch=<毫秒>` 与 `--watch <毫秒>` 都认。
     */
    if (argc > 1 && strcmp(argv[1], "--al") == 0)
    {
        if (argc != 2)
        {
            fprintf(stderr, "错误：--al 不接受参数，也不能与 --watch 同用\n");
            print_usage(stderr, argv[0]);
            return 2;
        }
        al_mode = true;
    }
    else if (argc > 1)
    {
        const char *value = NULL;
        bool consumed_next = false;
        const emaster_cli_option_result_t result = emaster_cli_option_match(
            argv[1], "--watch", (argc > 2) ? argv[2] : NULL, &value, &consumed_next);
        const int consumed = consumed_next ? 2 : 1;

        if (result == EMASTER_CLI_OPTION_MISSING)
        {
            fprintf(stderr, "错误：--watch 缺毫秒数\n");
            print_usage(stderr, argv[0]);
            return 2;
        }
        if (result != EMASTER_CLI_OPTION_VALUE || consumed != argc - 1)
        {
            fprintf(stderr, "错误：不认识的选项 '%s'\n",
                    (consumed + 1 < argc) ? argv[consumed + 1] : argv[1]);
            print_usage(stderr, argv[0]);
            return 2;
        }
        watch_ms = strtol(value, NULL, 10);
    }
    if (watch_ms < 0)
    {
        watch_ms = 0;
    }
    if (!install_signal_handlers())
    {
        fputs("错误：无法安装信号处理\n", stderr);
        return 1;
    }

    fd = connect_to_master(socket_path);
    if (fd < 0)
    {
        fprintf(stderr, "无法连接到主站：%s\n", socket_path);
        return 1;
    }

    if (al_mode)
    {
        char buffer[STATUS_RESPONSE_MAX];

        if (send_command(fd, "al\n", buffer, sizeof(buffer)) != 0)
        {
            close(fd);
            return 1;
        }
        print_al_response(buffer);
    }
    else if (watch_ms == 0)
    {
        char buffer[STATUS_RESPONSE_MAX];

        if (send_command(fd, "status\n", buffer, sizeof(buffer)) != 0)
        {
            close(fd);
            return 1;
        }
        status_frame_t frame;

        parse_frame(buffer, &frame);
        frame.truncated = frame_is_truncated(buffer);
        print_frame(&frame);
    }
    else
    {
        /*
         * 长连接复用同一条套接字：命令服务器一次只接受一个客户端，
         * 每次刷新都重连会和下一个刷新争抢连接。
         */
        while (!watch_stop)
        {
            char buffer[STATUS_RESPONSE_MAX];
            status_frame_t frame;
            struct timespec delay;

            if (send_command(fd, "status\n", buffer, sizeof(buffer)) != 0)
            {
                break;
            }
            parse_frame(buffer, &frame);
            frame.truncated = frame_is_truncated(buffer);
            /* 原地重绘：终端里长时间观察也不会把历史刷走。 */
            printf("\033[H\033[J");
            print_frame(&frame);
            (void)fflush(stdout);

            delay.tv_sec = watch_ms / 1000;
            delay.tv_nsec = (watch_ms % 1000) * 1000000L;
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR && !watch_stop)
            {
            }
        }
    }

    close(fd);
    return 0;
}
