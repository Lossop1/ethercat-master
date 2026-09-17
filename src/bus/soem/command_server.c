#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/command_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * 命令服务器实现：使用Unix域套接字和独立线程接收命令。
 * 设计要点：
 * - 监听线程负责accept和read，不阻塞周期线程
 * - 命令队列实现生产者-消费者模式
 * - 使用互斥锁保护共享状态
 */

#define COMMAND_QUEUE_CAPACITY 16

typedef struct {
    emaster_command_t commands[COMMAND_QUEUE_CAPACITY];
    size_t read_index;
    size_t write_index;
    size_t count;
} command_queue_t;

struct emaster_command_server {
    char socket_path[256];
    int listen_fd;
    int client_fd;
    pthread_t thread;
    pthread_mutex_t mutex;
    command_queue_t queue;
    bool running;
    emaster_command_server_stats_t stats;
};

/*
 * P9.4: 逐条命令的明细日志开关，默认关。
 *
 * 常态时不打：100 Hz 的外部目标流会让"每条命令两行"变成 200 行/秒，把主站日志淹掉，
 * 而这份日志是台架事后取证的主要材料。默认关、需要时用 EMASTER_CMD_SERVER_VERBOSE=1
 * 打开（沿用本仓其它开关的写法与取值集合）。
 *
 * 不用 session_internal.h 的 emaster_soem_env_flag_enabled：那个头把 SOEM 拖进来，
 * 而本文件现在是独立的（只依赖 libc 与自己的头）。为了一个十行的判断不值得把这条
 * 边界打破，所以这里自带一个同语义的静态函数——两边取值集合要一致，改动时一起改。
 */
static bool command_server_verbose_enabled(void)
{
    const char *value = getenv("EMASTER_CMD_SERVER_VERBOSE");

    if (value == NULL)
    {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "on") == 0 || strcmp(value, "ON") == 0 ||
           strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0;
}

/* 初始化命令队列 */
static void queue_init(command_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

/*
 * 这里原先有一个 queue_push（入队：生产者调用），从未被调用过——监听线程的入队路径
 * 需要在同一把锁里同时拿到"入队后的计数"快照，于是那份逻辑被就地展开写在了那里，
 * 这个函数从此只是编译器的 -Wunused-function 警告来源。删掉：一条长期存在的警告
 * 会让人对警告列表脱敏，将来真的出现新警告就看不见了。入队逻辑本身没动。
 */

/* 出队：消费者（周期线程）调用 */
static bool queue_pop(command_queue_t *queue, emaster_command_t *command)
{
    if (queue->count == 0U)
    {
        return false;  /* 队列空 */
    }
    *command = queue->commands[queue->read_index];
    queue->read_index = (queue->read_index + 1U) % COMMAND_QUEUE_CAPACITY;
    --queue->count;
    return true;
}

/* 客户端空闲多久后视为断开。单次操作的客户端不会保持连接超过数秒。 */
#define COMMAND_CLIENT_IDLE_TIMEOUT_S 5

/*
 * 客户端断开而主站仍持有 socket 时，read 不会返回：TCP/Unix 流套接字要等到写失败
 * 或收到 FIN 才能察觉。若此时客户端进程被 kill，连接会一直挂着，监听线程永远阻塞在
 * read 上，后续任何客户端都连不进来（accept 队列被占满，表现为 Connection refused）。
 * 用接收超时把这种「半开连接」变成可回收状态，使连接失败后仍能重新连接。
 */
static void set_client_timeout(int fd)
{
    struct timeval timeout;

    timeout.tv_sec = COMMAND_CLIENT_IDLE_TIMEOUT_S;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/* 监听线程：接受连接并读取命令 */
static void *listen_thread(void *arg)
{
    emaster_command_server_t *server = (emaster_command_server_t *)arg;
    char buffer[512];
    ssize_t bytes_read;
    size_t queue_count; /* 锁内快照，供解锁后的 fprintf 使用 */
    emaster_command_t command;
    /* 开关读一次就够：环境变量在进程生命周期内不变，逐条命令去查等于把
     * getenv + 若干 strcmp 放进 100 Hz 的热路径里。 */
    bool verbose = command_server_verbose_enabled();

    /*
     * 客户端可能在响应写出前关闭连接，此时 write 会触发 SIGPIPE 并终止整个主站。
     * 屏蔽该信号，写失败退化为 EPIPE，由调用者处理。
     */
    (void)signal(SIGPIPE, SIG_IGN);

    while (server->running)
    {
        /* 等待客户端连接（阻塞，需要互斥保护） */
        pthread_mutex_lock(&server->mutex);
        int current_client_fd = server->client_fd;
        pthread_mutex_unlock(&server->mutex);

        if (current_client_fd < 0)
        {
            int new_fd;

            new_fd = accept(server->listen_fd, NULL, NULL);
            if (new_fd < 0)
            {
                if (errno == EINTR)
                {
                    continue;  /* 信号中断，重试 */
                }
                break;  /* 接受失败 */
            }
            pthread_mutex_lock(&server->mutex);
            /*
             * 监听线程只会缓慢推进，但 client_fd 可能已被销毁路径或超时回收置为 -1；
             * 若仍有旧连接，先关闭再接管，避免 fd 泄漏。
             */
            if (server->client_fd >= 0 && server->client_fd != new_fd)
            {
                close(server->client_fd);
            }
            server->client_fd = new_fd;
            current_client_fd = new_fd;
            pthread_mutex_unlock(&server->mutex);
            set_client_timeout(new_fd);
        }

        /* 读取命令前再次验证fd有效性（防止TOCTOU） */
        pthread_mutex_lock(&server->mutex);
        if (server->client_fd != current_client_fd) {
            /* fd已被其他线程修改，重新循环 */
            pthread_mutex_unlock(&server->mutex);
            continue;
        }
        pthread_mutex_unlock(&server->mutex);

        /* 读取命令（阻塞，受 SO_RCVTIMEO 限制） */
        bytes_read = read(current_client_fd, buffer, sizeof(buffer) - 1U);
        if (bytes_read <= 0)
        {
            /* EAGAIN/EWOULDBLOCK 表示超时：客户端已不再持有连接，回收以便下次连接。 */
            bool timed_out = bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);

            pthread_mutex_lock(&server->mutex);
            if (server->client_fd == current_client_fd)
            {
                close(server->client_fd);
                server->client_fd = -1;
            }
            pthread_mutex_unlock(&server->mutex);
            if (timed_out)
            {
                fprintf(stderr, "[CMD_SERVER] Client idle for %ds, connection released\n",
                        COMMAND_CLIENT_IDLE_TIMEOUT_S);
                fflush(stderr);
            }
            continue;
        }

        buffer[bytes_read] = '\0';

        /* 解析命令（简单文本协议：第一个单词是命令类型） */
        memset(&command, 0, sizeof(command));

        /* 边界检查并精确匹配命令（带分隔符验证） */
        if (bytes_read >= 4 && strncmp(buffer, "stop", 4) == 0 &&
            (buffer[4] == '\n' || buffer[4] == '\0'))
        {
            command.type = EMASTER_COMMAND_STOP_MOTION;
        }
        else if (bytes_read >= 6 && strncmp(buffer, "status", 6) == 0 &&
                 (buffer[6] == '\n' || buffer[6] == '\0'))
        {
            command.type = EMASTER_COMMAND_QUERY_STATUS;
        }
        else if (bytes_read >= 8 && strncmp(buffer, "topology", 8) == 0 &&
                 (buffer[8] == '\n' || buffer[8] == '\0'))
        {
            command.type = EMASTER_COMMAND_QUERY_TOPOLOGY;
        }
        else if (bytes_read >= 8 && strncmp(buffer, "switch ", 7) == 0)
        {
            /* 注意：比较中包含空格分隔符 */
            command.type = EMASTER_COMMAND_SWITCH_MOTION;
            /* payload 包含motion profile ID */
            size_t payload_start = 7U;
            size_t payload_len = (size_t)bytes_read - payload_start;
            if (payload_len >= sizeof(command.payload))
            {
                payload_len = sizeof(command.payload) - 1U;
            }
            memcpy(command.payload, buffer + payload_start, payload_len);
            command.payload[payload_len] = '\0';
            command.payload[strcspn(command.payload, "\n")] = '\0';
        }
        else if (bytes_read >= 22 && strncmp(buffer, "set_external_target ", 20) == 0)
        {
            /* 注意：比较中包含空格分隔符 */
            command.type = EMASTER_COMMAND_SET_EXTERNAL_TARGET;
            /* payload 包含空格分隔的位置值 */
            size_t payload_start = 20U;
            size_t payload_len = (size_t)bytes_read - payload_start;
            if (payload_len >= sizeof(command.payload))
            {
                payload_len = sizeof(command.payload) - 1U;
            }
            memcpy(command.payload, buffer + payload_start, payload_len);
            command.payload[payload_len] = '\0';
            command.payload[strcspn(command.payload, "\n")] = '\0';
        }
        else if (bytes_read >= 8 && strncmp(buffer, "shutdown", 8) == 0 &&
                 (buffer[8] == '\n' || buffer[8] == '\0'))
        {
            command.type = EMASTER_COMMAND_SHUTDOWN;
        }
        else if (bytes_read >= 10 && strncmp(buffer, "quick_stop", 10) == 0 &&
                 (buffer[10] == '\n' || buffer[10] == '\0'))
        {
            command.type = EMASTER_COMMAND_QUICK_STOP;
        }
        else if (bytes_read >= 5 && strncmp(buffer, "halt ", 5) == 0)
        {
            command.type = EMASTER_COMMAND_HALT;
            size_t payload_start = 5U;
            size_t payload_len = (size_t)bytes_read - payload_start;
            if (payload_len >= sizeof(command.payload))
            {
                payload_len = sizeof(command.payload) - 1U;
            }
            memcpy(command.payload, buffer + payload_start, payload_len);
            command.payload[payload_len] = '\0';
            command.payload[strcspn(command.payload, "\n")] = '\0';
        }
        else if (bytes_read >= 11 && strncmp(buffer, "fault_reset", 11) == 0 &&
                 (buffer[11] == '\n' || buffer[11] == '\0'))
        {
            command.type = EMASTER_COMMAND_FAULT_RESET;
        }
        else
        {
            command.type = EMASTER_COMMAND_INVALID;
            /*
             * 认不出的一律计数；明细仍只在开关打开时打。这不是常态行——只有客户端
             * 发来不认识的字节才会出现——但它可以被打成常态（一个版本错配的客户端
             * 每拍发一条），所以它同样不能无条件写 stderr。
             */
            if (server->stats.invalid_count != UINT64_MAX)
            {
                ++server->stats.invalid_count;
            }
            if (verbose)
            {
                fprintf(stderr, "[CMD_SERVER] Unknown command, bytes_read=%zd\n", bytes_read);
                fflush(stderr);
            }
        }

        if (verbose)
        {
            fprintf(stderr, "[CMD_SERVER] Parsed command type=%d\n", command.type);
            fflush(stderr);
        }

        /* 入队（线程安全，完整原子性） */
        pthread_mutex_lock(&server->mutex);
        if (server->queue.count >= COMMAND_QUEUE_CAPACITY)
        {
            /* 队列满：发送错误响应给客户端，然后丢弃 */
            if (server->stats.queue_full_count != UINT64_MAX)
            {
                ++server->stats.queue_full_count;
            }
            pthread_mutex_unlock(&server->mutex);
            const char *error_msg = "ERROR|Command queue full\n";
            write(current_client_fd, error_msg, strlen(error_msg));
            /* 这一条不跟开关走：队列满说明周期线程没在取命令，是故障线索。 */
            fprintf(stderr, "警告：命令队列已满，丢弃命令\n");
        }
        else
        {
            /* 执行完整的入队操作：复制命令 + 更新索引 + 增加计数 */
            server->queue.commands[server->queue.write_index] = command;
            server->queue.write_index = (server->queue.write_index + 1U) % COMMAND_QUEUE_CAPACITY;
            ++server->queue.count;
            queue_count = server->queue.count;
            if (server->stats.received_count != UINT64_MAX)
            {
                ++server->stats.received_count;
            }
            pthread_mutex_unlock(&server->mutex);

            /*
             * 打印在**解锁之后**。这把锁的另一端是实时周期线程——它每拍调一次
             * emaster_command_server_receive 来取命令，而 stderr 若是管道或终端，
             * 一次 fprintf+fflush 可以阻塞任意久。持锁做 I/O 等于把这个无界等待
             * 转嫁给周期线程。计数先在锁内取快照，解锁后再打印，因此打印的仍然是
             * 那一刻的真实值。
             *
             * P9.4 起默认不打（见 command_server_verbose_enabled）：常态流量下这两行
             * 是日志体积的主项，"命令在不在流"改由报告里的 command_received_count 回答。
             */
            if (verbose)
            {
                fprintf(stderr, "[CMD_SERVER] Enqueued, count=%zu\n", queue_count);
                fflush(stderr);
            }
        }
    }

    return NULL;
}

emaster_command_server_t *emaster_command_server_create(const char *socket_path)
{
    emaster_command_server_t *server;
    struct sockaddr_un addr;
    int result;

    if (socket_path == NULL || strlen(socket_path) >= sizeof(addr.sun_path))
    {
        return NULL;
    }

    server = calloc(1U, sizeof(*server));
    if (server == NULL)
    {
        return NULL;
    }

    /* 初始化状态 */
    strncpy(server->socket_path, socket_path, sizeof(server->socket_path) - 1U);
    server->listen_fd = -1;
    server->client_fd = -1;
    server->running = true;
    queue_init(&server->queue);

    result = pthread_mutex_init(&server->mutex, NULL);
    if (result != 0)
    {
        free(server);
        return NULL;
    }

    /* 创建Unix域套接字 */
    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
    {
        pthread_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }

    /* 删除已存在的套接字文件 */
    (void)unlink(socket_path);

    /* 绑定地址 */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1U);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(server->listen_fd);
        pthread_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }

    /*
     * 主站通常以 root 启动（需要 CAP_NET_RAW 和 RT 调度），bind 出来的套接字
     * 归 root 所有且权限由 umask 决定（常见为 0755）。Unix 域套接字的 connect()
     * 要求对套接字文件有写权限，非 root 客户端会拿到 EACCES。
     * 这里显式放开为 0666，使台架上的普通用户能直接连控制接口。
     * 注意：这是刻意的安全取舍——任何能访问 /tmp 的本地用户都能控制电机。
     * 若部署环境有多个不可信本地用户，应改为 0660 并归于专用组。
     */
    if (chmod(socket_path, 0666) < 0)
    {
        fprintf(stderr, "警告：无法修改套接字权限 %s：%s\n", socket_path, strerror(errno));
    }

    /* 监听 */
    if (listen(server->listen_fd, 1) < 0)
    {
        close(server->listen_fd);
        unlink(socket_path);
        pthread_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }

    /* 启动监听线程 */
    result = pthread_create(&server->thread, NULL, listen_thread, server);
    if (result != 0)
    {
        close(server->listen_fd);
        unlink(socket_path);
        pthread_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }

    return server;
}

void emaster_command_server_destroy(emaster_command_server_t *server,
                                    emaster_command_server_stats_t *stats)
{
    if (server == NULL)
    {
        return;
    }

    /* 停止监听线程 */
    server->running = false;
    if (server->listen_fd >= 0)
    {
        /* 关闭监听套接字，让accept返回 */
        shutdown(server->listen_fd, SHUT_RDWR);
    }
    pthread_join(server->thread, NULL);

    /*
     * P9.4: 取数放在 join 之后、free 之前。写计数的是刚被 join 掉的那个线程，
     * 所以此刻读到的是它的全部写入（join 提供了同步），不需要再加锁。
     */
    if (stats != NULL)
    {
        *stats = server->stats;
    }

    /* 清理资源 */
    if (server->client_fd >= 0)
    {
        close(server->client_fd);
    }
    if (server->listen_fd >= 0)
    {
        close(server->listen_fd);
    }
    (void)unlink(server->socket_path);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}

bool emaster_command_server_receive(emaster_command_server_t *server,
                                   emaster_command_t *command)
{
    bool received;

    if (server == NULL || command == NULL)
    {
        return false;
    }

    pthread_mutex_lock(&server->mutex);
    received = queue_pop(&server->queue, command);
    pthread_mutex_unlock(&server->mutex);

    return received;
}

bool emaster_command_server_respond(emaster_command_server_t *server,
                                   const emaster_command_response_t *response)
{
    /* 响应缓冲区必须覆盖 message 全文，否则截断的多轴状态会被当成完整回读。 */
    char buffer[sizeof(response->message) + 16];
    ssize_t bytes_written;
    int len;
    int client_fd;

    if (server == NULL || response == NULL)
    {
        return false;
    }

    /*
     * 在锁内取 fd 快照：监听线程可能因超时关闭同一 fd，若在锁外读取并与 close
     * 竞争，写出的响应可能落到被复用的 fd 上。
     */
    pthread_mutex_lock(&server->mutex);
    client_fd = server->client_fd;
    pthread_mutex_unlock(&server->mutex);
    if (client_fd < 0)
    {
        return false;
    }

    /* 格式化响应：success|message */
    len = snprintf(buffer, sizeof(buffer), "%s|%s\n",
                   response->success ? "OK" : "ERROR",
                   response->message);
    if (len < 0 || (size_t)len >= sizeof(buffer))
    {
        return false;
    }

    /*
     * 发送响应。这段代码跑在**实时周期线程**上（session_control.c 的 status 分支每拍
     * 调一次），所以绝不能阻塞：客户端连上就不读 → 套接字发送缓冲写满 → 原来的阻塞
     * write() 会让周期线程卡在系统调用里，直到对方读取或断连。这不是理论风险，是
     * "任何客户端都能拖死控制回路"的现成通道。
     *
     * MSG_DONTWAIT 把"发不出去"变成立刻返回的错误。代价是可能出现**短写**：缓冲区
     * 只装得下一部分，`OK|<半个多轴状态>` 留在流里，后续响应接在它后面，客户端的行
     * 解析从此错位。所以短写与 EAGAIN 一并按"这个客户端跟不上"处理——关掉连接让它
     * 重连。响应以换行结尾，客户端读到的不完整行没有结尾换行，这是它与完整响应可
     * 区分的地方。
     *
     * 在锁内 close 并置 -1，与监听线程回收连接的既有写法一致（它也是"锁内改
     * client_fd"）。fd 复用窗口与监听线程自己 :146 的那处检查同级：需要监听线程
     * 恰好在这几微秒内走完"读失败→close→accept 拿到同一个 fd 号"，而它绝大部分
     * 时间阻塞在 read 上。
     */
    bytes_written = send(client_fd, buffer, (size_t)len, MSG_DONTWAIT);
    if (bytes_written == (ssize_t)len)
    {
        return true;
    }

    pthread_mutex_lock(&server->mutex);
    if (server->client_fd == client_fd)
    {
        close(server->client_fd);
        server->client_fd = -1;
    }
    pthread_mutex_unlock(&server->mutex);
    return false;
}

const char *emaster_command_server_socket_path(const emaster_command_server_t *server)
{
    return server != NULL ? server->socket_path : NULL;
}
