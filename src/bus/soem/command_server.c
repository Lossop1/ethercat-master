#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/command_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
};

/* 初始化命令队列 */
static void queue_init(command_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

/* 入队：生产者（监听线程）调用 */
static bool queue_push(command_queue_t *queue, const emaster_command_t *command)
{
    if (queue->count >= COMMAND_QUEUE_CAPACITY)
    {
        return false;  /* 队列满 */
    }
    queue->commands[queue->write_index] = *command;
    queue->write_index = (queue->write_index + 1U) % COMMAND_QUEUE_CAPACITY;
    ++queue->count;
    return true;
}

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
    emaster_command_t command;

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
            fprintf(stderr, "[CMD_SERVER] Unknown command, bytes_read=%zd\n", bytes_read);
            fflush(stderr);
        }

        fprintf(stderr, "[CMD_SERVER] Parsed command type=%d\n", command.type);
        fflush(stderr);

        /* 入队（线程安全，完整原子性） */
        pthread_mutex_lock(&server->mutex);
        if (server->queue.count >= COMMAND_QUEUE_CAPACITY)
        {
            /* 队列满：发送错误响应给客户端，然后丢弃 */
            pthread_mutex_unlock(&server->mutex);
            const char *error_msg = "ERROR|Command queue full\n";
            write(current_client_fd, error_msg, strlen(error_msg));
            fprintf(stderr, "警告：命令队列已满，丢弃命令\n");
        }
        else
        {
            /* 执行完整的入队操作：复制命令 + 更新索引 + 增加计数 */
            server->queue.commands[server->queue.write_index] = command;
            server->queue.write_index = (server->queue.write_index + 1U) % COMMAND_QUEUE_CAPACITY;
            ++server->queue.count;
            fprintf(stderr, "[CMD_SERVER] Enqueued, count=%zu\n", server->queue.count);
            fflush(stderr);
            pthread_mutex_unlock(&server->mutex);
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

void emaster_command_server_destroy(emaster_command_server_t *server)
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

    /* 发送响应；客户端已断开时返回 EPIPE（SIGPIPE 已屏蔽），不影响主站运行。 */
    bytes_written = write(client_fd, buffer, (size_t)len);
    return bytes_written == len;
}

const char *emaster_command_server_socket_path(const emaster_command_server_t *server)
{
    return server != NULL ? server->socket_path : NULL;
}
