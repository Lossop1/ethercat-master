#!/usr/bin/env python3
"""把主站的两个 AF_UNIX 套接字转发成 TCP，供另一台机器上的界面使用。

**为什么必须有这一层。** 主站的命令口与观测口都是 AF_UNIX，路径写死在唯一构造点
（include/emaster/bus/command_socket_path.h）。路径名到不了另一台机器，而且整个仓库
没有任何 AF_INET / socat / ssh -L 的现成通路。所以"在 Windows 上跑界面、主站在香橙
派上"这件事，中间必须有东西转发——就是这个脚本。

    Windows                                    Orange Pi
    tool --tcp pi:5001  ──TCP──►  本脚本 ──unix──► /tmp/emaster-<id>.sock
                                  监听 5001/5002   /tmp/emaster-<id>-obs.sock

**上游也走端点写法**，所以本机没有 AF_UNIX（Windows）时可以用 `--tcp` 指一个 TCP
上游，把它当纯搬运器来测。这不是为了好玩：这个脚本里唯一会出错的东西就是搬运本身
（半条回复、写不动、连接换手），而那部分在台架之前总得能验一遍。

**两侧的"第二个客户端"语义不一样，本脚本照原样保留：**

- 命令口：真主站只 accept 一个，第二个客户端连得上却永远拿不到回复（挂住）。桥这里
  **明确回一条 ERROR 再关**——比挂住强，且对规矩的客户端而言后果相同（就是拿不到第二条
  命令通道）。这是有意偏离真主站的一处，写在这里免得日后被当成 bug。
- 观测口：真主站是**新连接顶掉旧的**（server.c:489）。桥照做：踢掉旧的下游连接。

**换手时上游一定重连。** 理由不是洁癖：上游是字节流，一个客户端在 DUMP 回包只读了
一半时被换掉，剩下那半截仍在上游缓冲里——新客户端的第一条 INFO 就会收到那半截，从此
永远错位。换手时把上游一起换掉，流的位置就跟着归零。

**资源隔离（用户定的）**：这一层不许和主站抢 CPU。做法是把自己绑到主站**用不着**的核上，
并在开局、每 5 秒、以及每次有新客户端时重新对照一次 `/proc`——因为桥可能先于主站启动。
不做这件事的话，主站的三个实时线程和搬运线程会落在同一个核上排队。

**暴露面**：这两个口没有任何认证（主站的命令套接字本来就是 0666，本地任何用户都能驱动
电机）。桥把它扩大到了局域网上——默认只绑 127.0.0.1 就是为了这件事，跨机器必须显式
`--bind`，且启动时打一行警告。
"""

import argparse
import errno
import os
import selectors
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emaster_endpoint  # noqa: E402
import rt_affinity  # noqa: E402

# 每条连接一次最多搬多少。32 KB 是"够大不至于空转、够小不至于让另一向饿着"的折中：
# 一次搬完一整条满窗口 DUMP（约 45 KB）要两轮，另一向的等待不会超过一两次循环。
CHUNK = 32768

# 允许在下游方向积压多少字节。超了就把下游踢掉。
#
# **为什么必须有这个上限。** 桥是主动抽干上游的（上游有多快就搬多快），所以上游永远
# 不会对主站造成反压——这本身是对的，真主站也明确不给自己反压。代价是：一个连上就
# 不读的客户端，字节全堆在桥的待发缓冲里。真主站对这种客户端的处置是**断开**
# （server.c 的背压路径），桥不设上限的话就变成"默默吃内存"，在香橙派上迟早出事，
# 而且症状是整机变慢，离原因很远。
#
# 1 MB 的取法：界面按 20 Hz 取 64 帧（5 轴，11 KB/轮），1 MB ≈ 90 轮积压——正常
# 客户端永远碰不到；而一个真不读的客户端几十轮就越过去了。
MAX_BUFFERED = 1 << 20

# ---------------------------------------------------------------- CPU 亲和
#
# 实现在 tools/rt_affinity.py，图形界面也用同一份。这里转发几个名字只是让调用点
# 读起来短。
AffinityKeeper = rt_affinity.AffinityKeeper
parse_cpu_list = rt_affinity.parse_cpu_list


# ---------------------------------------------------------------- 一条通道

class Link:
    """一个监听口 + 一条上游连接 + 一条下游连接，双向搬运。"""

    def __init__(self, name, listener, upstream_endpoint, replace_downstream, log):
        self.name = name
        self.listener = listener
        self.upstream_endpoint = upstream_endpoint
        self.replace_downstream = replace_downstream
        self.log = log

        self.downstream = None
        self.upstream = None
        self.to_upstream = bytearray()
        self.to_downstream = bytearray()

        self.counters = {
            "accepted": 0,
            "rejected_busy": 0,
            "replaced": 0,
            "upstream_connects": 0,
            "upstream_eof": 0,
            "downstream_eof": 0,
            "upstream_errors": 0,
            "downstream_stalled": 0,
            "bytes_up": 0,
            "bytes_down": 0,
            "max_buffered_downstream": 0,
        }

    # ---------- 连接管理 ----------

    def _close_downstream(self, reason):
        if self.downstream is None:
            return
        self.log(f"{self.name}：断开下游（{reason}）")
        _shutdown(self.downstream)
        self.downstream = None
        self.to_downstream = bytearray()

    def _close_upstream(self, reason):
        if self.upstream is None:
            return
        _shutdown(self.upstream)
        self.upstream = None
        self.to_upstream = bytearray()
        self.counters["upstream_eof"] += 1

    def drop_all(self, reason):
        """换手或出错：两侧都断，流的位置跟着归零（见模块开头）。"""
        self._close_downstream(reason)
        self._close_upstream(reason)

    def handle_accept(self):
        try:
            conn, peer = self.listener.accept()
        except (BlockingIOError, InterruptedError):
            return
        except OSError as error:
            if error.errno not in (errno.EMFILE, errno.ENFILE, errno.ECONNABORTED):
                self.log(f"{self.name}：accept 出错（{error}）")
            return

        if self.downstream is not None and not self.replace_downstream:
            # 命令口：真主站让第二个客户端挂住。这里明确说一句再关——比挂着强，
            # 且对规矩的客户端后果相同（就是没有第二条命令通道）。
            self.counters["rejected_busy"] += 1
            self.log(f"{self.name}：已有客户端，拒绝 {peer[0]}:{peer[1]}")
            _send_and_close(
                conn,
                f"ERROR|{self.name}已经有客户端了（主站只支持一个），"
                f"请先断开那一个\n")
            return

        if self.downstream is not None:
            self.counters["replaced"] += 1
            self.log(f"{self.name}：新连接 {peer[0]}:{peer[1]} 顶掉旧连接")
            self.drop_all("被新连接顶替")
        else:
            self.counters["accepted"] += 1
            self.log(f"{self.name}：{peer[0]}:{peer[1]} 连上")

        conn.setblocking(False)
        self.downstream = conn
        # 下游一换，上游也换：半条回复留在上游缓冲里就会让新客户端永远错位。
        self._connect_upstream()

    def _connect_upstream(self):
        try:
            self.upstream = self.upstream_endpoint.connect(5.0)
        except (OSError, emaster_endpoint.EndpointError) as error:
            self.counters["upstream_errors"] += 1
            self.log(f"{self.name}：连不上上游 {self.upstream_endpoint.display}：{error}")
            self._close_downstream("上游连不上")
            return
        self.upstream.setblocking(False)
        self.counters["upstream_connects"] += 1
        self.log(f"{self.name}：上游已连 {self.upstream_endpoint.display}")

    # ---------- 搬运 ----------

    def sockets(self):
        for sock in (self.listener, self.downstream, self.upstream):
            if sock is not None:
                yield sock

    def interest(self, sock):
        events = selectors.EVENT_READ
        if sock is self.downstream and self.to_upstream:
            events |= selectors.EVENT_WRITE
        if sock is self.upstream and self.to_downstream:
            events |= selectors.EVENT_WRITE
        return events

    def _read_into(self, sock, buffer):
        try:
            chunk = sock.recv(CHUNK)
        except (BlockingIOError, InterruptedError):
            return True
        except OSError:
            return False
        if not chunk:
            return False
        buffer += chunk
        return True

    def _write_from(self, sock, buffer):
        if not buffer:
            return True
        try:
            sent = sock.send(buffer)
        except (BlockingIOError, InterruptedError):
            return True                      # 等下一轮，不是错
        except OSError:
            return False
        del buffer[:sent]
        return True

    def handle_downstream(self, mask):
        if mask & selectors.EVENT_READ:
            if not self._read_into(self.downstream, self.to_upstream):
                self.counters["downstream_eof"] += 1
                # 下游走了：上游也一起换掉，免得把半条回复留给下一个客户端。
                self.drop_all("下游关闭")
                return
        if mask & selectors.EVENT_WRITE and self.downstream is not None:
            if not self._write_from(self.downstream, self.to_downstream):
                self.counters["downstream_eof"] += 1
                self.drop_all("下游写不动")
                return
        if self.to_upstream and self.upstream is not None:
            before = len(self.to_upstream)
            if not self._write_from(self.upstream, self.to_upstream):
                self.counters["upstream_errors"] += 1
                self.drop_all("上游写不动")
                return
            self.counters["bytes_up"] += before - len(self.to_upstream)
            if self.to_upstream and len(self.to_upstream) == before:
                # 上游写不动了（EAGAIN 连续）。等下一轮，别忙转。
                return

    def handle_upstream(self, mask):
        if mask & selectors.EVENT_READ:
            if not self._read_into(self.upstream, self.to_downstream):
                # 上游关了（真主站 5 秒空闲就会踢）。下游跟着断，让客户端如实看到
                # "连接没了"并能自己重连——桥不替它假装还活着。
                self.counters["upstream_eof"] += 1
                self.drop_all("上游关闭")
                return
            if len(self.to_downstream) > MAX_BUFFERED:
                # 下游只连不读。真主站对这种客户端是断开的，桥照做——见 MAX_BUFFERED
                # 那段：差别只在"断开"与"默默吃内存"之间，而后者在香橙派上的表现
                # 是整机变慢，离原因很远。
                self.counters["downstream_stalled"] += 1
                self.log(f"{self.name}：下游积压超过 {MAX_BUFFERED} 字节"
                         f"（只连不读），断开")
                self.drop_all("下游只连不读")
                return
        if mask & selectors.EVENT_WRITE and self.upstream is not None:
            if not self._write_from(self.upstream, self.to_upstream):
                self.counters["upstream_errors"] += 1
                self.drop_all("上游写不动")
                return
        if self.to_downstream and self.downstream is not None:
            before = len(self.to_downstream)
            if not self._write_from(self.downstream, self.to_downstream):
                self.counters["downstream_eof"] += 1
                self.drop_all("下游写不动")
                return
            self.counters["bytes_down"] += before - len(self.to_downstream)
        if len(self.to_downstream) > self.counters["max_buffered_downstream"]:
            self.counters["max_buffered_downstream"] = len(self.to_downstream)


def _shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def _send_and_close(sock, text):
    try:
        sock.sendall(text.encode("utf-8"))
    except OSError:
        pass
    _shutdown(sock)


# ---------------------------------------------------------------- 主循环

def serve(links, log, keeper):
    selector = selectors.DefaultSelector()
    for link in links:
        selector.register(link.listener, selectors.EVENT_READ, link)

    running = True
    try:
        while running:
            for key, mask in selector.select(1.0):
                link = key.data
                if key.fileobj is link.listener:
                    keeper.reconcile()
                    link.handle_accept()
                elif key.fileobj is link.downstream:
                    link.handle_downstream(mask)
                else:
                    link.handle_upstream(mask)
                resync(selector, link)
            keeper.reconcile()
    except KeyboardInterrupt:
        log("收到中断，收摊")
    finally:
        selector.close()
        for link in links:
            link.drop_all("退出")


def resync(selector, link):
    """连接来去之后重新登记这组套接字。"""
    live = set(link.sockets())
    for sock in list(selector.get_map().values()):
        if sock.data is link and sock.fileobj not in live:
            try:
                selector.unregister(sock.fileobj)
            except (KeyError, OSError, ValueError):
                pass
    for sock in live:
        try:
            selector.unregister(sock)
        except (KeyError, OSError, ValueError):
            pass
        try:
            selector.register(sock, link.interest(sock), link)
        except (OSError, ValueError):
            pass


def _listen(host, port):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, port))
    listener.listen(4)
    listener.setblocking(False)
    return listener


def main():
    global MAX_BUFFERED
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    emaster_endpoint.add_endpoint_arguments(parser, command=True, observation=True)
    parser.add_argument("--bind", default="127.0.0.1",
                        help="监听地址。默认 127.0.0.1（只本机）；跨机器要显式给管理口 IP")
    parser.add_argument("--port", type=int,
                        default=emaster_endpoint.DEFAULT_TCP_COMMAND_PORT,
                        help="命令口监听端口")
    parser.add_argument("--obs-port", type=int, default=None,
                        help="观测口监听端口（默认命令口 +1）")
    parser.add_argument("--cpu", default="auto",
                        help="本进程绑到哪些核，如 0-9；auto = 躲开主站线程占的核")
    parser.add_argument("--max-buffered", type=int, default=MAX_BUFFERED,
                        help=f"下游方向最多积压多少字节，超了断开（默认 {MAX_BUFFERED}）")
    parser.add_argument("--quiet", action="store_true", help="只打警告和错误")
    args = parser.parse_args()
    MAX_BUFFERED = args.max_buffered

    def log(message):
        if not args.quiet:
            stamp = time.strftime("%H:%M:%S")
            print(f"[{stamp}] {message}", flush=True)

    def warn(message):
        stamp = time.strftime("%H:%M:%S")
        print(f"[{stamp}] 警告：{message}", flush=True)

    # --tcp 在桥的语境里指的是**上游**，这一点与其它工具不同，要说清楚。
    endpoints = emaster_endpoint.resolve_endpoints(
        args, parser, need=("command", "observation"))

    if args.bind not in ("127.0.0.1", "localhost", "::1"):
        warn(f"监听在 {args.bind}，这两个口**没有任何认证**："
             f"能连上命令口的人就能驱动电机。确认这是你要的。")

    requested = None
    if args.cpu.strip().lower() != "auto":
        requested = parse_cpu_list(args.cpu)
        if not requested:
            parser.error(f"--cpu 解不动：{args.cpu!r}（写成 0-9 或 0,2,4-7）")
    keeper = AffinityKeeper(requested, log)
    keeper.reconcile(force=True)

    obs_port = args.obs_port if args.obs_port is not None else args.port + 1
    cmd_link = Link("命令口", _listen(args.bind, args.port),
                    endpoints["command"], replace_downstream=False, log=log)
    obs_link = Link("观测口", _listen(args.bind, obs_port),
                    endpoints["observation"], replace_downstream=True, log=log)

    print(f"上游命令口 {endpoints['command'].display}", flush=True)
    print(f"上游观测口 {endpoints['observation'].display}", flush=True)
    print(f"监听 {args.bind}:{args.port}（命令）/ {args.bind}:{obs_port}（观测）",
          flush=True)
    print("Ctrl-C 停", flush=True)

    try:
        serve([cmd_link, obs_link], log, keeper)
    finally:
        for link in (cmd_link, obs_link):
            print(f"{link.name}：{link.counters}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
