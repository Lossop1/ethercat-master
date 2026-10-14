#define _POSIX_C_SOURCE 200809L

#include "emaster/setpoint/server.h"

#include "emaster/thread_sched.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/*
 * 设点套接字服务器。与观测口同一条铁律：**任何一处都不得等待客户端**。
 *
 * 这条通道写进的是周期路径唯一的输入。一旦它能在某处等客户端，客户端的一帧迟到就
 * 变成主站的一次抖动，而"客户端多久发一次"是客户端的事。因此这里没有回写、没有
 * 响应、没有重试；收到的记录当场写进缓冲区就走。
 *
 * 接收缓冲按"最大合法记录 + 1"定长。多出来的那一个字节是判据：SOCK_SEQPACKET 会把
 * 超出缓冲的消息截断后再回长度，所以长度正好等于容量必然是超长消息，整条拒收，而不是
 * 拿一个被截断的尾部当合法数据。
 */

/* poll 的超时。只用于让停止请求在有限时间内被看到，不参与任何协议语义。 */
#define SETPOINT_POLL_TIMEOUT_MS 200

#define SETPOINT_RX_CAPACITY (EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES + 1U)

struct emaster_setpoint_server
{
    char socket_path[108];
    int listen_fd;
    int client_fd;
    volatile bool running;
    pthread_t thread;
    /*
     * 汇点。所有权不在本结构体：它由 main 持有，同时被周期回调读取，两者用 buffer
     * 自己的互斥锁同步。这里只借指针。
     */
    emaster_external_target_buffer_t *buffer;
    size_t axis_count;
    int8_t expected_modes[EMASTER_SETPOINT_WIRE_MAX_AXES];
    /* 只有监听线程访问，所以不需要锁。 */
    uint8_t rx[SETPOINT_RX_CAPACITY];
    emaster_setpoint_stats_t stats;
    bool have_last_sequence;
    /* 最后一条已采纳记录的到达时刻（主站单调时钟），销毁时用来算年龄。 */
    uint64_t last_accepted_ns;
};

/* 单调时钟，纳秒。用于记录到达时刻与收尾时的年龄，不参与协议语义。 */
static uint64_t setpoint_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * 记一次拒收。
 *
 * 只在第一次打印。这一条会按客户端频率重复（客户端发的格式不对就会一直不对），每帧
 * 打一行会把主站日志淹掉，而那正是要留给人看的东西。之后的次数进报告与计数器。
 */
static void note_reject(emaster_setpoint_server_t *server,
                        emaster_setpoint_reject_t reason, size_t axis)
{
    server->stats.records_rejected += 1U;
    server->stats.last_reject_reason = reason;
    server->stats.last_reject_axis = axis;
    if (server->stats.records_rejected == 1U)
    {
        fprintf(stderr, "[SETPOINT] 拒收设点记录：%s（第 %zu 轴）；会话继续\n",
                emaster_setpoint_reject_text(reason), axis + 1U);
    }
}

/* 把一条已通过校验的记录搬进缓冲区。整条一起写，中途不释放锁。 */
static void store_record(emaster_setpoint_server_t *server,
                         const emaster_setpoint_record_t *record)
{
    emaster_external_target_buffer_t *buffer = server->buffer;
    uint64_t now_ns = setpoint_now_ns();
    size_t axis_index;

    pthread_mutex_lock(&buffer->mutex);
    for (axis_index = 0U; axis_index < server->axis_count; ++axis_index)
    {
        const emaster_setpoint_axis_t *axis = &record->axes[axis_index];

        buffer->positions[axis_index] = axis->q;
        buffer->modes[axis_index] = axis->mode;
        buffer->velocities[axis_index] = axis->dq;
        buffer->torques[axis_index] = axis->tau;
        buffer->kp[axis_index] = axis->kp;
        buffer->kd[axis_index] = axis->kd;
    }
    buffer->sequence = record->sequence;
    buffer->client_time_ns = record->client_time_ns;
    buffer->last_update_ns = now_ns;
    buffer->available = 1;
    pthread_mutex_unlock(&buffer->mutex);

    /*
     * 序号缺口：只在序号**前进**时算。回退或重复说明客户端重启或重放，那不是丢包，
     * 计进去只会把"丢了多少条"这个数弄脏。
     */
    if (server->have_last_sequence && record->sequence > server->stats.last_sequence)
    {
        server->stats.sequence_gaps += record->sequence - server->stats.last_sequence - 1U;
    }
    server->have_last_sequence = true;
    server->stats.last_sequence = record->sequence;
    server->stats.last_client_time_ns = record->client_time_ns;
    server->stats.records_accepted += 1U;
    server->stats.last_record_age_ns = 0;
    server->last_accepted_ns = now_ns;
}

/* 校验并采纳（或拒收）一条刚收到的记录。 */
static void handle_record(emaster_setpoint_server_t *server, size_t length)
{
    emaster_setpoint_record_t record;
    size_t axis_index;

    if (length >= (size_t)SETPOINT_RX_CAPACITY)
    {
        note_reject(server, EMASTER_SETPOINT_REJECT_TRUNCATED, 0U);
        return;
    }
    if (!emaster_setpoint_wire_decode(server->rx, length, &record))
    {
        note_reject(server, EMASTER_SETPOINT_REJECT_BAD_FORMAT, 0U);
        return;
    }
    /*
     * 轴数必须与本部署完全相等。少一条就不知道省下的是哪一轴，多一条就是给不存在的
     * 轴发目标——两种都不能靠猜补上，整条拒。
     */
    if ((size_t)record.axis_count != server->axis_count)
    {
        note_reject(server, EMASTER_SETPOINT_REJECT_AXIS_COUNT, 0U);
        return;
    }
    /*
     * 模式核对是 mode 这个字段今天唯一被"消费"的地方。同一条记录在不同模式下语义
     * 完全不同（CSP 的 q 是目标位置，MIT 的 q 是参考位置、力矩由 kp/kd 算），模式
     * 对不上就是把数按错的语义写进输出路径。
     */
    for (axis_index = 0U; axis_index < server->axis_count; ++axis_index)
    {
        if (server->expected_modes[axis_index] != EMASTER_SETPOINT_MODE_UNCHECKED &&
            record.axes[axis_index].mode != server->expected_modes[axis_index])
        {
            note_reject(server, EMASTER_SETPOINT_REJECT_MODE_MISMATCH, axis_index);
            return;
        }
    }

    store_record(server, &record);
}

/*
 * 关掉并回收当前客户端。只有监听线程调用，因此不需要锁——client_fd 也只在监听线程
 * 里被读写。这与命令口用互斥保护 client_fd 不同：那里 fd 会被周期线程用来回响应，
 * 这里没有任何第二个线程碰它。
 */
static void drop_client(emaster_setpoint_server_t *server)
{
    if (server->client_fd >= 0)
    {
        (void)close(server->client_fd);
        server->client_fd = -1;
    }
}

static void *setpoint_thread(void *arg)
{
    emaster_setpoint_server_t *server = (emaster_setpoint_server_t *)arg;

    /*
     * 本线程从主线程继承了 SCHED_FIFO 与绑核。它做的是 socket 收包与一次加锁拷贝，
     * 都不是实时工作，占着实时优先级只会让它与周期线程同优先级互不抢占，等于把
     * 非实时负载传导到周期路径上——而那正是这条通道设计上要避免的事。
     * 理由与内核实测见 emaster/thread_sched.h。
     */
    (void)emaster_thread_leave_realtime("设点服务线程");

    while (server->running)
    {
        struct pollfd fds[2];
        nfds_t count = 1U;
        int ready;

        fds[0].fd = server->listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (server->client_fd >= 0)
        {
            fds[1].fd = server->client_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            count = 2U;
        }

        ready = poll(fds, count, SETPOINT_POLL_TIMEOUT_MS);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (ready == 0)
        {
            continue; /* 超时：回到循环顶检查 running */
        }

        if ((fds[0].revents & POLLIN) != 0)
        {
            int new_fd = accept(server->listen_fd, NULL, NULL);

            if (new_fd >= 0)
            {
                /*
                 * 单客户端模型：接管新连接前先断开旧的。不这样做的话，客户端重启后
                 * 旧连接会一直占着，而两个客户端同时写同一个汇点没有任何仲裁依据。
                 */
                drop_client(server);
                server->client_fd = new_fd;
            }
        }

        /*
         * 必须拿 fds[1].fd 与当前 client_fd 比对再动手，不能直接 recv(client_fd)：
         * 同一轮 poll 里可能刚 accept 并顶掉了旧的，此时 fds[1] 描述的是**旧**连接的
         * 事件，对旧连接的 POLLHUP 会让下面这句 drop_client() 把刚接管的新客户端一并
         * 丢掉——表现为"一连上就被断"，且只在旧连接断开与新连接到达撞在同一轮时出现。
         */
        if (count == 2U && fds[1].fd == server->client_fd &&
            (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
            ssize_t bytes_read;

            if ((fds[1].revents & (POLLHUP | POLLERR)) != 0 &&
                (fds[1].revents & POLLIN) == 0)
            {
                drop_client(server);
                continue;
            }
            /*
             * SOCK_SEQPACKET：一次 recv 正好一条消息，边界由内核给。缓冲比最大合法
             * 记录多一个字节，所以"回满"就是超长，交给 handle_record 判。
             */
            bytes_read = recv(server->client_fd, server->rx, sizeof(server->rx), 0);
            if (bytes_read <= 0)
            {
                /* 0 = 对端正常关闭；<0 = 出错或不可读。两种都回收连接。 */
                if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                {
                    continue;
                }
                drop_client(server);
                continue;
            }
            handle_record(server, (size_t)bytes_read);
        }
    }

    return NULL;
}

emaster_setpoint_server_t *
emaster_setpoint_server_create(const char *socket_path,
                               emaster_external_target_buffer_t *buffer,
                               size_t axis_count,
                               const int8_t *expected_modes)
{
    emaster_setpoint_server_t *server;
    struct sockaddr_un addr;
    size_t axis_index;

    if (socket_path == NULL || buffer == NULL || axis_count == 0U ||
        axis_count > (size_t)EMASTER_SETPOINT_WIRE_MAX_AXES)
    {
        return NULL;
    }
    if (strlen(socket_path) >= sizeof(addr.sun_path))
    {
        return NULL;
    }

    server = calloc(1U, sizeof(*server));
    if (server == NULL)
    {
        return NULL;
    }
    (void)snprintf(server->socket_path, sizeof(server->socket_path), "%s", socket_path);
    server->listen_fd = -1;
    server->client_fd = -1;
    server->running = true;
    server->buffer = buffer;
    server->axis_count = axis_count;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        server->expected_modes[axis_index] =
            (expected_modes != NULL) ? expected_modes[axis_index] : EMASTER_SETPOINT_MODE_UNCHECKED;
    }
    server->stats.enabled = true;
    server->stats.last_record_age_ns = -1;

    /* SOCK_SEQPACKET：消息边界由内核给，粘包与半条记录这一类问题在这里不存在。 */
    server->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (server->listen_fd < 0)
    {
        fprintf(stderr, "警告：设点套接字建立失败 %s：%s\n", socket_path, strerror(errno));
        free(server);
        return NULL;
    }

    /* 上一个进程留下的套接字文件会让 bind 报 EADDRINUSE，先清掉。 */
    (void)unlink(socket_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "警告：设点套接字绑定失败 %s：%s\n", socket_path, strerror(errno));
        (void)close(server->listen_fd);
        free(server);
        return NULL;
    }

    /*
     * 与另两口同一取舍：主站以 root 启动，bind 出来的套接字默认非 root 客户端连不上。
     * 这一口**能改电机目标**，放开到 0666 的风险与命令口同级。多用户部署应改为 0660
     * 并归于专用组——这一点写在 README 的使用边界里。
     */
    if (chmod(socket_path, 0666) < 0)
    {
        fprintf(stderr, "警告：无法修改设点套接字权限 %s：%s\n", socket_path, strerror(errno));
    }

    if (listen(server->listen_fd, 1) < 0)
    {
        (void)close(server->listen_fd);
        (void)unlink(socket_path);
        free(server);
        return NULL;
    }

    if (pthread_create(&server->thread, NULL, setpoint_thread, server) != 0)
    {
        (void)close(server->listen_fd);
        (void)unlink(socket_path);
        free(server);
        return NULL;
    }

    fprintf(stderr, "[SETPOINT] 设点通道已开：%s（%zu 轴，只写不回）\n",
            socket_path, axis_count);
    return server;
}

void emaster_setpoint_server_destroy(emaster_setpoint_server_t *server,
                                     emaster_setpoint_stats_t *stats)
{
    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
        stats->last_record_age_ns = -1;
    }
    if (server == NULL)
    {
        return;
    }

    server->running = false;
    /*
     * shutdown 让阻塞在 poll 上的监听线程立刻醒来；没有它最坏要等一个 poll 超时。
     * 只关 fd 而不 shutdown 是不安全的——fd 号可能被别的线程复用，poll 就会盯上一个
     * 完全无关的描述符。
     */
    if (server->listen_fd >= 0)
    {
        (void)shutdown(server->listen_fd, SHUT_RDWR);
    }
    (void)pthread_join(server->thread, NULL);

    /*
     * 线程已经停了，此后不会再有记录到达，所以这份快照就是整轮的数字；在这里算年龄
     * 也不存在"读的时候监听线程正在写"的窗口——这正是把取数与销毁合成一步的理由。
     */
    if (server->stats.records_accepted > 0U)
    {
        uint64_t now_ns = setpoint_now_ns();

        server->stats.last_record_age_ns =
            (now_ns >= server->last_accepted_ns) ? (int64_t)(now_ns - server->last_accepted_ns) : 0;
    }
    if (stats != NULL)
    {
        *stats = server->stats;
    }

    drop_client(server);
    if (server->listen_fd >= 0)
    {
        (void)close(server->listen_fd);
    }
    (void)unlink(server->socket_path);
    free(server);
}

const char *emaster_setpoint_server_socket_path(const emaster_setpoint_server_t *server)
{
    return server != NULL ? server->socket_path : NULL;
}
