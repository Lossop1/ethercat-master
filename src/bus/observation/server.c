#define _POSIX_C_SOURCE 200809L

#include "emaster/observation/server.h"

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
 * 观测套接字服务器。**唯一一条铁律：任何一处都不得等待客户端。**
 *
 * 这不是"尽量不阻塞"的软要求，而是这个模块存在的理由。命令通道可以阻塞——它承载的
 * 是控制意图，慢一点是功能问题；观测通道一旦能阻塞，它就多了一条把非实时负载传导到
 * 主站的路径，而观测本该是完全旁路的。因此：
 *
 * - 写走 MSG_DONTWAIT，重试有上界（OBS_WRITE_ATTEMPTS），用尽即关掉该客户端；
 * - 读走非阻塞 + poll，从不做阻塞 read；
 * - 周期线程从不调用本文件的任何函数，它连这个结构体的指针都拿不到。
 *
 * "连上但永不读"的客户端因此只能伤到它自己：套接字缓冲写满 → 重试耗尽 → 连接被回收。
 */

/*
 * 一次响应的写重试上限。4 次 × 每次间隔 1 ms = 最坏 4 ms，且**不占任何共享资源**：
 * 这段时间里周期线程照常发帧，环形缓冲照常被覆盖（这正是容量 256 存在的意义——
 * 4 ms 只覆盖 4 帧）。用尽即关闭，让"不读的客户端"退化为"连接被断开"。
 */
#define OBS_WRITE_ATTEMPTS 4U
#define OBS_WRITE_RETRY_DELAY_NS 1000000L

/*
 * 一次响应的**总**时间上限。只有"连续 EAGAIN 次数"这一道不够：每有一点进展就把重试
 * 预算清零，于是每 10 ms 读走一个字节的客户端可以让监听线程永远停在 write 上，新客户端
 * 再也接不进来（监听线程是单线程，accept 也在它里面）。有进展是好事，但不能无限期。
 *
 * 20 ms 是"够慢的正常客户端也不该被误杀"与"不能拖住监听线程"之间的取值：一份满窗口
 * DUMP 是 39 KB，正常本地客户端在 µs 级写完。
 */
#define OBS_WRITE_DEADLINE_NS 20000000L

/* 单条文本命令的长度上限。动词最长是 "DUMP 123456 123456"，128 字节绰绰有余。 */
#define OBS_RX_CAPACITY 128U

/* poll 的超时。只用于让停止请求在有限时间内被看到，不参与任何协议语义。 */
#define OBS_POLL_TIMEOUT_MS 200

struct emaster_observation_server
{
    char socket_path[108];
    int listen_fd;
    int client_fd;
    volatile bool running;
    pthread_t thread;
    /*
     * 环形缓冲的所有权不在本结构体：它是只读视图，生命周期由会话保证。本结构体
     * 持有的是 const 指针，编译期就挡住"服务器去写缓冲区"这类改动。
     */
    const emaster_observation_ring_t *ring;
    emaster_observation_info_t info;
    /* DUMP 的拼包缓冲。只有监听线程用它，所以不需要锁。 */
    uint8_t *tx_buffer;
    size_t tx_capacity;
    /* 客户端命令行缓冲。同上，只有监听线程访问。 */
    char rx_buffer[OBS_RX_CAPACITY];
    size_t rx_length;
};

/* ---- 写出 ------------------------------------------------------------------- */

/* 单调时钟，纳秒。只用于给一次响应定超时上界，不参与协议语义。 */
static uint64_t observation_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * 把 buffer 全部写出。返回是否成功。
 *
 * 用 send(MSG_DONTWAIT | MSG_NOSIGNAL) 而不是 write() + 全进程 signal(SIGPIPE, SIG_IGN)：
 * 信号处置是进程级的，从观测线程去改它会影响别的线程（命令服务器已经在改一次了）。
 * MSG_NOSIGNAL 把这件事收进这一次调用里。
 */
static bool write_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0U;
    unsigned attempt = 0U;
    bool deadline_armed = false;
    uint64_t deadline_ns = 0U;

    while (sent < length)
    {
        ssize_t written = send(fd, buffer + sent, length - sent, MSG_DONTWAIT | MSG_NOSIGNAL);

        if (written > 0)
        {
            uint64_t now_ns;

            sent += (size_t)written;
            attempt = 0U; /* 有进展就重置重试预算，慢客户端不该被误判为不读 */
            /*
             * 总时限第一次遇到 EAGAIN 时才起算。从函数入口起算会让"已经写了 20 ms 的
             * 大响应"在没有任何阻塞的情况下被判超时——那是对正常客户端的误杀，而这里
             * 要限制的是**等待**，不是工作量。
             */
            if (deadline_armed)
            {
                now_ns = observation_now_ns();
                if (now_ns - deadline_ns > (uint64_t)OBS_WRITE_DEADLINE_NS)
                {
                    return false;
                }
            }
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct timespec delay;

            if (!deadline_armed)
            {
                deadline_armed = true;
                deadline_ns = observation_now_ns();
            }
            if (++attempt >= OBS_WRITE_ATTEMPTS)
            {
                return false;
            }
            delay.tv_sec = 0;
            delay.tv_nsec = OBS_WRITE_RETRY_DELAY_NS;
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        return false; /* 对端已关闭（EPIPE/ECONNRESET）或其它硬错误 */
    }
    return true;
}

static bool write_text(int fd, const char *text)
{
    return write_all(fd, (const uint8_t *)text, strlen(text));
}

/* ---- 各动词 ----------------------------------------------------------------- */

static bool reply_info(const emaster_observation_server_t *server, int fd)
{
    char line[512];
    int written = snprintf(line, sizeof(line),
                           "OK|wire_version=%u|deployment=%s|interface=%s|axes=%u|stride=%u"
                           "|capacity=%u|frame_bytes=%u|axis_bytes=%u|verbs=INFO,HEAD,LATEST,DUMP\n",
                           (unsigned)EMASTER_OBSERVATION_WIRE_VERSION,
                           server->info.deployment_id, server->info.interface_name,
                           (unsigned)server->info.axis_count, (unsigned)server->info.stride,
                           (unsigned)server->info.ring_capacity,
                           (unsigned)emaster_observation_wire_frame_bytes(server->info.axis_count),
                           (unsigned)EMASTER_OBSERVATION_WIRE_AXIS_BYTES);

    if (written < 0 || (size_t)written >= sizeof(line))
    {
        return false;
    }
    return write_text(fd, line);
}

static bool reply_head(const emaster_observation_server_t *server, int fd)
{
    char line[256];
    uint64_t oldest = 0U;
    uint64_t newest = 0U;
    int written;

    if (!emaster_observation_ring_window(server->ring, &oldest, &newest))
    {
        /* 还没有任何帧发布过。这不是错误，客户端据此知道要等。 */
        written = snprintf(line, sizeof(line), "OK|head=0|empty=1|capacity=%u\n",
                           (unsigned)server->info.ring_capacity);
    }
    else
    {
        written = snprintf(line, sizeof(line),
                           "OK|oldest=%llu|newest=%llu|capacity=%u\n",
                           (unsigned long long)oldest, (unsigned long long)newest,
                           (unsigned)server->info.ring_capacity);
    }
    if (written < 0 || (size_t)written >= sizeof(line))
    {
        return false;
    }
    return write_text(fd, line);
}

/*
 * LATEST：最新一帧的文本视图。
 *
 * 形状与命令通道的 status 响应一致（顶层 key=value 以空格分隔、'|' 分段、轴段 "aN:"
 * 前缀、段内 ',' 分隔），字段名也一致（pos/vel/torque/status/target_pos）。因此
 * tools/status_client 能直接解析——同一份解析器、同一个显示，只是数据来源从"边读边
 * 格式化"换成了"一次原子快照"。
 *
 * 刻意**不**提供 state=/enabled=/completed=：帧是一份数据快照，不是控制状态报告，
 * 主站状态在会话里而不在环形缓冲里。status_client 的解析器对未知键与缺失键都是容错的
 * （缺失字段显示为 "-"），所以少这几个键不影响它工作；需要控制状态的客户端应当连命令
 * 通道。把主站状态塞进每一帧也只是白占带宽——它每拍都在变，而帧的用途是给策略当观测。
 */
static bool reply_latest(const emaster_observation_server_t *server, int fd)
{
    char line[2048];
    emaster_observation_frame_t frame;
    uint64_t head = emaster_observation_ring_head(server->ring);
    int offset;
    uint16_t axis_index;

    if (head == 0U)
    {
        return write_text(fd, "OK|empty=1\n");
    }
    if (emaster_observation_ring_read(server->ring, head - UINT64_C(1), &frame) !=
        EMASTER_OBSERVATION_READ_OK)
    {
        /* 写者正好越过这一格：让客户端重问，比返回一份半截快照好。 */
        return write_text(fd, "ERROR|unstable\n");
    }

    offset = snprintf(line, sizeof(line),
                      "OK|publish_index=%llu cycle=%llu mono_ns=%llu interval_ns=%llu wkc=%d"
                      " axes=%u flags=0x%08x",
                      (unsigned long long)frame.publish_index,
                      (unsigned long long)frame.cycle,
                      (unsigned long long)frame.monotonic_ns,
                      (unsigned long long)frame.frame_interval_ns, (int)frame.wkc,
                      (unsigned)frame.axis_count, (unsigned)frame.flags);
    for (axis_index = 0U; axis_index < frame.axis_count; ++axis_index)
    {
        const emaster_observation_axis_t *axis = &frame.axes[axis_index];
        int written;

        if (offset < 0 || (size_t)offset >= sizeof(line))
        {
            break;
        }
        written = snprintf(line + offset, sizeof(line) - (size_t)offset,
                           "|a%u:pos=%d,vel=%d,torque=%d,status=0x%04x,target_pos=%d,"
                           "control=0x%04x,flags=0x%08x",
                           (unsigned)(axis_index + 1U), axis->actual_position,
                           axis->actual_velocity, (int)axis->actual_torque,
                           (unsigned)axis->status_word, axis->target_position,
                           (unsigned)axis->control_word, (unsigned)axis->flags);
        if (written < 0)
        {
            break;
        }
        offset += written;
    }
    if (offset < 0 || (size_t)offset >= sizeof(line) - 1U)
    {
        return write_text(fd, "ERROR|truncated\n");
    }
    line[offset] = '\n';
    line[offset + 1] = '\0';
    return write_text(fd, line);
}

/*
 * DUMP <from> <count>：回一段定长小端二进制帧，**前面带一个事务头**。
 *
 * 能取到多少就回多少——请求区间里一旦有一帧已被覆盖，就在那里停下，并在事务头里如实
 * 报告返回的帧数与起始序号。为什么不停下来返回一个错误：策略要的是一个**时序连续**的
 * 历史窗口，头部把区间讲清楚，客户端自己判断窗口够不够；返回错误会让它除了重试别无
 * 选择，而重试拿到的窗口只会更短。绝不跳过中间的空洞继续回——那会让客户端以为拿到的
 * 是连续序列，是最坏的失真。
 *
 * 事务头**无论有没有帧都要发**，包括空窗口。它是客户端唯一的"这段到哪结束"依据；
 * 缺了它，客户端在长连接上读完最后一帧就只能一直等下去（理由见 wire.h 的 KIND_DUMP）。
 */
static bool reply_dump(emaster_observation_server_t *server, int fd, uint64_t from,
                       uint64_t count)
{
    uint64_t capacity = (uint64_t)server->info.ring_capacity;
    uint64_t head = emaster_observation_ring_head(server->ring);
    uint64_t available = 0U;
    uint64_t index;
    uint64_t first_index = 0U;
    uint32_t frame_count = 0U;
    size_t header_bytes = 0U;
    /* 帧区从事务头之后开始，最后再把头回填到开头——遍历中途停下时帧数才知道。 */
    size_t offset = (size_t)EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES;

    if (count > (uint64_t)EMASTER_OBSERVATION_MAX_DUMP_FRAMES)
    {
        count = (uint64_t)EMASTER_OBSERVATION_MAX_DUMP_FRAMES;
    }
    /* 可读窗口与请求区间取交集，一次算清楚，免得逐帧撞 STALE。 */
    if (head > 0U)
    {
        uint64_t oldest = head >= capacity ? head - capacity + UINT64_C(1) : 0U;

        if (from < oldest)
        {
            from = oldest;
        }
        if (from < head)
        {
            available = head - from;
            if (available > count)
            {
                available = count;
            }
        }
    }

    for (index = from; index < from + available; ++index)
    {
        emaster_observation_frame_t frame;
        size_t written = 0U;

        if (emaster_observation_ring_read(server->ring, index, &frame) !=
            EMASTER_OBSERVATION_READ_OK)
        {
            /*
             * 在第一个空洞处停下。跳过空洞继续回会让客户端以为拿到的是**连续**序列，
             * 而策略是按连续序列算 dt 和时序特征的——这是最坏的一类失真。宁可短。
             */
            break;
        }
        if (!emaster_observation_wire_encode_frame(&frame, server->tx_buffer + offset,
                                                   server->tx_capacity - offset, &written))
        {
            break;
        }
        if (frame_count == 0U)
        {
            first_index = index;
        }
        offset += written;
        ++frame_count;
    }

    if (!emaster_observation_wire_encode_dump(first_index, frame_count,
                                              server->info.axis_count, server->tx_buffer,
                                              (size_t)EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES,
                                              &header_bytes) ||
        header_bytes != (size_t)EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES)
    {
        return write_text(fd, "ERROR|dump header encode failed\n");
    }
    return write_all(fd, server->tx_buffer, offset);
}

/* 解析并执行一条命令行。成功派发返回 true；无法识别返回 false（调用方回错误行）。 */
static bool dispatch(emaster_observation_server_t *server, int fd, const char *line)
{
    if (strcmp(line, "INFO") == 0)
    {
        return reply_info(server, fd);
    }
    if (strcmp(line, "HEAD") == 0)
    {
        return reply_head(server, fd);
    }
    if (strcmp(line, "LATEST") == 0)
    {
        return reply_latest(server, fd);
    }
    if (strncmp(line, "DUMP ", 5) == 0)
    {
        unsigned long long from = 0ULL;
        unsigned long long count = 0ULL;

        if (sscanf(line + 5, "%llu %llu", &from, &count) != 2)
        {
            return write_text(fd, "ERROR|usage: DUMP <from> <count>\n");
        }
        return reply_dump(server, fd, (uint64_t)from, (uint64_t)count);
    }
    return write_text(fd, "ERROR|unknown verb; try INFO, HEAD, LATEST, DUMP <from> <count>\n");
}

/* 处理客户端缓冲区里已经凑齐的整行。返回是否继续保留该连接。 */
static bool consume_lines(emaster_observation_server_t *server, int fd)
{
    size_t start = 0U;
    size_t scan;

    for (scan = 0U; scan < server->rx_length; ++scan)
    {
        if (server->rx_buffer[scan] != '\n')
        {
            continue;
        }
        server->rx_buffer[scan] = '\0';
        /* 容忍 \r\n，客户端跨平台时省一次调试。 */
        if (scan > start && server->rx_buffer[scan - 1U] == '\r')
        {
            server->rx_buffer[scan - 1U] = '\0';
        }
        if (!dispatch(server, fd, server->rx_buffer + start))
        {
            return false;
        }
        start = scan + 1U;
    }
    if (start > 0U)
    {
        memmove(server->rx_buffer, server->rx_buffer + start, server->rx_length - start);
        server->rx_length -= start;
    }
    /* 缓冲区满了还没有换行：不是本协议的客户端，断开而不是无限增长。 */
    return server->rx_length < sizeof(server->rx_buffer);
}

/*
 * 关掉并回收当前客户端。只有监听线程调用，因此不需要锁——client_fd 也只在监听线程
 * 里被读写。这与 command_server 用互斥保护 client_fd 不同：那里的 fd 会被周期线程
 * 用来回响应，这里没有任何第二个线程碰它。
 */
static void drop_client(emaster_observation_server_t *server)
{
    if (server->client_fd >= 0)
    {
        (void)close(server->client_fd);
        server->client_fd = -1;
    }
    server->rx_length = 0U;
}

static void *observe_thread(void *arg)
{
    emaster_observation_server_t *server = (emaster_observation_server_t *)arg;

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

        ready = poll(fds, count, OBS_POLL_TIMEOUT_MS);
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
                 * 单客户端模型：接管新连接前先断开旧的。不这样做的话，交替连接的
                 * 客户端会各自占一个 fd，而它们看到的永远是同一个只读缓冲。
                 */
                drop_client(server);
                server->client_fd = new_fd;
            }
        }

        /*
         * 必须拿 fds[1].fd 与当前 client_fd 比对再动手，不能直接 recv(client_fd)：
         * 同一个 poll 轮次里可能刚 accept 了新连接并顶掉了旧的，此时 fds[1] 描述的是
         * **旧**连接的事件，而对旧连接的 POLLHUP 会让下面这句 drop_client() 把刚接管的
         * 新客户端一起丢掉——客户端表现为"一连上就被断"，且只在旧连接断开与新连接到达
         * 撞在同一轮时才出现。比对一下，事件与连接对不上就整段跳过，下一轮自然会重来。
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
            bytes_read = recv(server->client_fd, server->rx_buffer + server->rx_length,
                              sizeof(server->rx_buffer) - server->rx_length, 0);
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
            server->rx_length += (size_t)bytes_read;
            if (!consume_lines(server, server->client_fd))
            {
                drop_client(server);
            }
        }
    }

    return NULL;
}

emaster_observation_server_t *
emaster_observation_server_create(const char *socket_path,
                                  const emaster_observation_ring_t *ring,
                                  const emaster_observation_info_t *info)
{
    emaster_observation_server_t *server;
    struct sockaddr_un addr;

    if (socket_path == NULL || ring == NULL || info == NULL || info->axis_count == 0U ||
        info->axis_count > EMASTER_OBSERVATION_MAX_AXES)
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
    server->ring = ring;
    server->info = *info;
    server->info.axis_count = info->axis_count;
    server->info.ring_capacity = info->ring_capacity;
    /*
     * 拼包缓冲按"最坏情况 = 事务头 + 满窗口、每帧满轴数"预留，建会话时一次分配。
     * 让它随部署轴数伸缩，而不是一律按上限 16 轴。
     */
    server->tx_capacity = (size_t)EMASTER_OBSERVATION_WIRE_DUMP_HEADER_BYTES +
                          emaster_observation_wire_frame_bytes(info->axis_count) *
                              (size_t)EMASTER_OBSERVATION_MAX_DUMP_FRAMES;
    server->tx_buffer = malloc(server->tx_capacity);
    if (server->tx_buffer == NULL)
    {
        free(server);
        return NULL;
    }

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
    {
        free(server->tx_buffer);
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
        fprintf(stderr, "警告：观测套接字绑定失败 %s：%s\n", socket_path, strerror(errno));
        (void)close(server->listen_fd);
        free(server->tx_buffer);
        free(server);
        return NULL;
    }

    /*
     * 与命令套接字同一取舎：主站以 root 启动，bind 出来的套接字默认非 root 客户端连
     * 不上。观测通道只读，放开到 0666 的风险低于命令通道（后者能改目标位置），但仍
     * 意味着任何本地用户都能读到关节状态。多用户部署应改为 0660 并归于专用组。
     */
    if (chmod(socket_path, 0666) < 0)
    {
        fprintf(stderr, "警告：无法修改观测套接字权限 %s：%s\n", socket_path, strerror(errno));
    }

    if (listen(server->listen_fd, 1) < 0)
    {
        (void)close(server->listen_fd);
        (void)unlink(socket_path);
        free(server->tx_buffer);
        free(server);
        return NULL;
    }

    if (pthread_create(&server->thread, NULL, observe_thread, server) != 0)
    {
        (void)close(server->listen_fd);
        (void)unlink(socket_path);
        free(server->tx_buffer);
        free(server);
        return NULL;
    }
    return server;
}

void emaster_observation_server_destroy(emaster_observation_server_t *server)
{
    if (server == NULL)
    {
        return;
    }
    server->running = false;
    /*
     * shutdown 让阻塞在 poll 上的监听线程立刻醒来；没有它最坏要等一个 poll 超时。
     * 关 fd 而不 shutdown 是不安全的——fd 号可能被别的线程复用，poll 就会盯着一个
     * 完全无关的描述符。
     */
    if (server->listen_fd >= 0)
    {
        (void)shutdown(server->listen_fd, SHUT_RDWR);
    }
    (void)pthread_join(server->thread, NULL);

    drop_client(server);
    if (server->listen_fd >= 0)
    {
        (void)close(server->listen_fd);
    }
    (void)unlink(server->socket_path);
    free(server->tx_buffer);
    free(server);
}

const char *emaster_observation_server_socket_path(const emaster_observation_server_t *server)
{
    return server != NULL ? server->socket_path : NULL;
}
