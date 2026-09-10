#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/command_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* 监听线程：接受连接并读取命令 */
static void *listen_thread(void *arg)
{
    emaster_command_server_t *server = (emaster_command_server_t *)arg;
    char buffer[512];
    ssize_t bytes_read;
    emaster_command_t command;

    while (server->running)
    {
        /* 等待客户端连接（阻塞，需要互斥保护） */
        pthread_mutex_lock(&server->mutex);
        int current_client_fd = server->client_fd;
        pthread_mutex_unlock(&server->mutex);

        if (current_client_fd < 0)
        {
            int new_fd = accept(server->listen_fd, NULL, NULL);
            if (new_fd < 0)
            {
                if (errno == EINTR)
                {
                    continue;  /* 信号中断，重试 */
                }
                break;  /* 接受失败 */
            }
            pthread_mutex_lock(&server->mutex);
            server->client_fd = new_fd;
            current_client_fd = new_fd;
            pthread_mutex_unlock(&server->mutex);
        }

        /* 读取命令前再次验证fd有效性（防止TOCTOU） */
        pthread_mutex_lock(&server->mutex);
        if (server->client_fd != current_client_fd) {
            /* fd已被其他线程修改，重新循环 */
            pthread_mutex_unlock(&server->mutex);
            continue;
        }
        pthread_mutex_unlock(&server->mutex);

        /* 读取命令（阻塞） */
        bytes_read = read(current_client_fd, buffer, sizeof(buffer) - 1U);
        if (bytes_read <= 0)
        {
            /* 连接关闭或错误 */
            pthread_mutex_lock(&server->mutex);
            if (server->client_fd == current_client_fd)
            {
                close(server->client_fd);
                server->client_fd = -1;
            }
            pthread_mutex_unlock(&server->mutex);
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
    char buffer[512];
    ssize_t bytes_written;
    int len;

    if (server == NULL || response == NULL || server->client_fd < 0)
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

    /* 发送响应 */
    bytes_written = write(server->client_fd, buffer, (size_t)len);
    return bytes_written == len;
}

const char *emaster_command_server_socket_path(const emaster_command_server_t *server)
{
    return server != NULL ? server->socket_path : NULL;
}
