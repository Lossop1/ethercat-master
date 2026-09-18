#!/usr/bin/env python3
"""验证转发桥：TCP ↔ 上游 的双向搬运没有把字节流搞坏。

**为什么能在这台机器上验。** 上游走的是端点层，所以本机（Windows，没有 AF_UNIX）
可以让桥指一个 TCP 上游——对着 scripts/checks/fake_master.py。台架上上游是 unix，
但搬运代码是同一份：会出错的地方（半条回复、换手时的残余字节、写不动、
上游断开下游不跟）在两种上游下长得一样。这个检查就是冲着那几处去的。

真跑一遍数字更有说服力：一帧 176 字节 × 256 帧的满窗口 DUMP ≈ 45 KB，跨两次
CHUNK 搬运。只要中间错一个字节，客户端那套"任何不自洽都不猜"的解码就会当场拒绝，
不会安静地给出错位的关节角——所以"解出来了"本身就是搬运没坏的证据。

跑法：python scripts/checks/socket_bridge_check.py
"""

import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fake_master  # noqa: E402
import emaster_client  # noqa: E402


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


def free_port_block(length=4):
    """找一段**连续**的空闲端口，返回基址（占用 base .. base+length-1）。

    为什么非要连续、非要自己探："观测口 = 命令口 + 1" 是全仓的约定，主站和桥各自
    需要一对相邻端口。此前逐个 bind(0) 拿号——内核给的号往往就是连着的，于是桥的
    命令口正好落在主站的观测口上；Windows 的 SO_REUSEADDR 允许抢占这种正在用的
    端口，**绑定时一声不响**，然后桥的上游指向了自己的下游，症状是"命令发出去没有
    任何回应"。本机与台架都只有这一处会踩，所以在这里一次性把号段定死。
    """
    for _ in range(200):
        holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        holder.bind(("127.0.0.1", 0))
        base = holder.getsockname()[1]
        holder.close()
        if base + length > 65535:
            continue
        probes = []
        try:
            for offset in range(1, length):
                probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                probe.bind(("127.0.0.1", base + offset))
                probes.append(probe)
        except OSError:
            for probe in probes:
                probe.close()
            continue
        for probe in probes:
            probe.close()
        return base
    raise CheckFailed("找不到连续的空闲端口段")


class Bridge:
    """把 tools/socket_bridge.py 当子进程起来。"""

    def __init__(self, upstream_port, listen_port, listen_obs_port,
                 max_buffered=None, obs_upstream_port=None):
        self.upstream_port = upstream_port
        self.listen_port = listen_port
        self.listen_obs_port = listen_obs_port
        self.max_buffered = max_buffered
        self.obs_upstream_port = obs_upstream_port
        self.process = None
        self.output = []

    def __enter__(self):
        command = [sys.executable, os.path.join(ROOT, "tools", "socket_bridge.py"),
                   "--tcp", f"127.0.0.1:{self.upstream_port}",
                   "--port", str(self.listen_port),
                   "--obs-port", str(self.listen_obs_port)]
        if self.obs_upstream_port is not None:
            command += ["--obs-endpoint", f"tcp:127.0.0.1:{self.obs_upstream_port}"]
        if self.max_buffered is not None:
            command += ["--max-buffered", str(self.max_buffered)]
        command += ["--quiet"]
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=ROOT, text=True, encoding="utf-8", errors="replace",
        )
        # 等两个监听口真的能连上。不睡足就 connect 会拿到 ConnectionRefused，
        # 而那个错误看起来与"桥没起来"一样，容易误判。
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise CheckFailed(f"桥进程提前退出：{self.drain()}")
            if self._port_open(self.listen_obs_port):
                return self
            time.sleep(0.05)
        raise CheckFailed(f"桥 10 秒内没起来：{self.drain()}")

    def _port_open(self, port):
        try:
            sock = socket.create_connection(("127.0.0.1", port), 0.5)
        except OSError:
            return False
        sock.close()
        return True

    def drain(self):
        if self.process is None:
            return ""
        try:
            return self.process.stdout.read() or ""
        except (OSError, ValueError):
            return ""

    def __exit__(self, *_):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()


def _ask(sock, command):
    sock.sendall((command + "\n").encode())
    return _line(sock)


def _line(sock):
    data = bytearray()
    while True:
        byte = sock.recv(1)
        if not byte:
            raise CheckFailed(f"回复中途断了（收到 {bytes(data)!r}）")
        if byte == b"\n":
            return data.decode("utf-8", "replace")
        data += byte


def _read_some(sock, timeout):
    """读到 EOF 或超时。返回读到的字节（EOF 时是 b""）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return b"still-open"
        except (ConnectionResetError, OSError):
            return b""
        if not chunk:
            return b""
    return b"still-open"


def check_observation_pump(context):
    """满窗口 DUMP 跨桥搬过来，解码要完全自洽 —— 这是搬运没坏的主证据。"""
    base = free_port_block(4)
    master_port, bridge_port = base, base + 2
    with fake_master.FakeMaster(port=master_port, obs_port=master_port + 1,
                                publish_hz=1000.0) as master:
        with Bridge(master_port, bridge_port, bridge_port + 1):
            client = emaster_client.ObservationClient(
                f"tcp:127.0.0.1:{bridge_port + 1}")
            try:
                client.connect()
                client.read_info()
                want(client.axis_count == fake_master.DEFAULT_AXES,
                     f"经桥拿到的 INFO 不对：{client.info_fields}")

                deadline = time.monotonic() + 5.0
                newest = 0
                while time.monotonic() < deadline:
                    _, newest, _, empty = client.read_head()
                    if not empty and newest > fake_master.RING_CAPACITY + 8:
                        break
                    time.sleep(0.05)
                want(newest > fake_master.RING_CAPACITY,
                     f"经桥没等到环满（newest={newest}）")

                # 满窗口：256 帧 × 176 字节 ≈ 45 KB，跨数次 CHUNK。错一个字节，
                # 客户端那套自洽校验就会拒绝。
                first, frames = client.read_window(0, 256)
                want(len(frames) >= 200,
                     f"满窗口只搬过来 {len(frames)} 帧")
                indices = [f["publish_index"] for f in frames]
                want(indices[0] == first,
                     f"事务头起点 {first} 与首帧序号 {indices[0]} 不一致")
                holes = sum(1 for a, b in zip(indices, indices[1:]) if b != a + 1)
                want(holes == 0, f"经桥之后出现 {holes} 处跳号")

                # 反复取：搬运是有状态的，错位会随着轮次累积而暴露。
                for _ in range(20):
                    latest, axes = client.read_latest()
                    want(latest is not None, "经桥 LATEST 取不到帧")
                client.read_info()      # 长连接里插进文本动词，再证明流没错位
            finally:
                client.disconnect()


def check_command_pump(context):
    base = free_port_block(4)
    master_port, bridge_port = base, base + 2
    with fake_master.FakeMaster(port=master_port, obs_port=master_port + 1) as master:
        with Bridge(master_port, bridge_port, bridge_port + 1):
            sock = socket.create_connection(("127.0.0.1", bridge_port), 5.0)
            sock.settimeout(5.0)
            try:
                want(_ask(sock, "status").startswith("OK"),
                     "经桥的 status 没回 OK")
                # 双前缀必须原样穿过桥——桥要是有任何"按行修补"的动作就会丢一层。
                reply = _ask(sock, "set_external_target 0 0 0 0 0")
                want(reply.startswith("OK|OK|"),
                     f"经桥之后双前缀没了：{reply!r}")
                counters = master.snapshot_counters()
                want(counters["dropped_tail"] == 0,
                     f"经桥之后出现了被丢弃的命令：{counters}")
                want(counters["commands"] == 2,
                     f"经桥之后命令计数不对：{counters}")
            finally:
                sock.close()


def check_command_port_rejects_second_client(context):
    """命令口：真主站让第二个客户端挂住。桥明确回一条 ERROR——比挂着强，
    且对规矩的客户端后果相同（拿不到第二条命令通道）。"""
    base = free_port_block(4)
    master_port, bridge_port = base, base + 2
    with fake_master.FakeMaster(port=master_port, obs_port=master_port + 1):
        with Bridge(master_port, bridge_port, bridge_port + 1):
            first = socket.create_connection(("127.0.0.1", bridge_port), 5.0)
            first.settimeout(5.0)
            try:
                want(_ask(first, "status").startswith("OK"), "第一个客户端该能用")
                second = socket.create_connection(("127.0.0.1", bridge_port), 5.0)
                second.settimeout(5.0)
                try:
                    reply = _line(second)
                    want(reply.startswith("ERROR"),
                         f"第二个客户端应当收到一条明确的 ERROR：{reply!r}")
                finally:
                    second.close()
                # 拒绝了第二个之后，第一个必须还活着。
                want(_ask(first, "status").startswith("OK"),
                     "拒绝第二个客户端时把第一个也弄坏了")
            finally:
                first.close()


def check_observation_replaces_and_resyncs(context):
    """观测口换手：新客户端顶掉旧的，并且**流的相位要归零**。

    这是本检查里最要紧的一条。换手时若只换下游、不换上游，旧客户端那条只读了一半
    的回复会把字节留给新客户端，从此每条回复都错位。所以换手之后不光要能连上，
    还要能解出**自洽**的 DUMP —— 只验"能连上"是验不出来的。
    """
    base = free_port_block(4)
    master_port, bridge_port = base, base + 2
    with fake_master.FakeMaster(port=master_port, obs_port=master_port + 1,
                                publish_hz=1000.0) as master:
        with Bridge(master_port, bridge_port, bridge_port + 1):
            old = emaster_client.ObservationClient(f"tcp:127.0.0.1:{bridge_port + 1}")
            try:
                old.connect()
                old.read_info()
                # 故意让旧客户端停在一条满窗口 DUMP 的请求上：请求已经发出去，
                # 回复正在路上。这正是换手最容易留下残渣的时刻。
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline:
                    if not old.read_head()[3]:
                        break
                    time.sleep(0.05)
                old.sock.sendall(b"DUMP 0 256\n")
                time.sleep(0.05)        # 让上游开始往回搬

                new = emaster_client.ObservationClient(
                    f"tcp:127.0.0.1:{bridge_port + 1}")
                try:
                    new.connect()
                    new.read_info()
                    # 相位归零的直接判据：新客户端第一条 DUMP 就必须完全自洽。
                    # 旧回复的残渣还在的话，这里会解出魔数/长度不符而抛协议错。
                    first, frames = new.read_window(0, 64)
                    want(frames, "换手之后新客户端取不到帧")
                    indices = [f["publish_index"] for f in frames]
                    want(indices[0] == first,
                         f"换手之后事务头与首帧序号对不上：{first} vs {indices[0]}")
                    holes = sum(1 for a, b in
                                zip(indices, indices[1:]) if b != a + 1)
                    want(holes == 0, f"换手之后出现 {holes} 处跳号（残渣没清）")
                    # 再来一轮，证明不是"第一次碰巧"。
                    first2, frames2 = new.read_window(first, 64)
                    want(all(b == a + 1 for a, b in
                             zip([f["publish_index"] for f in frames2],
                                 [f["publish_index"] for f in frames2][1:])),
                         "换手之后第二轮出现跳号")
                finally:
                    new.disconnect()
            finally:
                old.disconnect()
            # 判据是**上游被重连过**（accept 到两次以上），而不是"主站看到过顶替"：
            # 桥是先关上游再连新的，主站多半已经把旧连接的 EOF 处理完、active 归了零，
            # 于是那次新连接只是"第一个客户端"，压根不计进顶替数。这里要钉的是设计
            # 意图——换手时上游必须跟着换（否则半个回复的残渣会留给新客户端），
            # 而 accept 次数正是这件事的直接证据。
            counters = master.snapshot_counters()
            want(counters["obs_accepts"] >= 2,
                 f"换手时上游没有重连（accept 次数不足）：{counters}")


def check_upstream_loss_closes_downstream(context):
    """上游没了（真主站 5 秒空闲就踢），下游必须跟着断。

    桥不许替客户端假装连接还在——界面要靠"连接断了"这个事实去重连，而一个永远不
    回复也不报错的通道，在界面上与"主站卡住了"完全一样。
    """
    base = free_port_block(4)
    master_port, bridge_port = base, base + 2
    master = fake_master.FakeMaster(port=master_port, obs_port=master_port + 1,
                                    idle_timeout=0.5)
    with master:
        with Bridge(master_port, bridge_port, bridge_port + 1):
            sock = socket.create_connection(("127.0.0.1", bridge_port), 5.0)
            sock.settimeout(8.0)
            try:
                want(_ask(sock, "status").startswith("OK"), "空闲前该能用")
                # 上游会在 0.5 秒空闲后关掉本连接。桥读到 EOF 后必须把下游也关掉。
                sock.sendall(b"status\n")
                data = _read_some(sock, 5.0)
                want(data == b"",
                     f"上游断掉之后下游应当也被断开，却收到 {data[:60]!r}")
            finally:
                sock.close()


class Firehose:
    """一个只会往外倒字节的上游，倒到对端关掉为止。

    为什么需要它：桥的积压上限和**真主站自己的**短写断开是两件事，而真主站会先动手
    （试过，最后断言里 `clients_kicked_unread == 1`，说明断开的功劳是主站的，不是桥
    的）。要单独验桥的那条路，就得有个不会主动踢人的上游挡在中间。
    """

    def __init__(self, port=0):
        # 端口由调用方指定，**不能**让它自己 bind(0) 去要：内核有相当大概率把
        # free_port_block 预留的那一段里的某个号分给它（本机实测就撞上了），于是
        # 它和桥自己的监听口落到同一个号上——Windows 的 SO_REUSEADDR 允许共用，
        # 绑定时不报错，客户端连过去被 firehose 接走，表现是"读到一堆 x"。
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = None
        self.peer_went_away = False
        self.connected = False

    def __enter__(self):
        import threading

        def pump():
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connected = True
            payload = b"x" * 65536
            try:
                while True:
                    conn.sendall(payload)
            except OSError:
                # 对端（桥）把这条上游关了。这正是"换手时上游也换"或"积压上限触发
                # 后断开下游"时会发生的事。
                self.peer_went_away = True
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

        self.thread = threading.Thread(target=pump, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        try:
            self.listener.close()
        except OSError:
            pass


def check_stalled_client_is_disconnected(context):
    """只连不读的客户端要被桥的上限断开，而不是让桥默默吃内存。

    **上游用 Firehose，不用假主站**：假主站在自己的发送缓冲写不下时会先断开，把桥
    这条路盖住（这条是实测出来的——带主站的版本最后报的是
    `clients_kicked_unread: 1`，即功劳是主站的）。用一个只倒字节、从不主动踢人的
    上游，断开的动作就只可能来自桥的上限。
    """
    # 上限故意调得比生产默认（1 MB）小得多：这里验的是**机制**（积压超限就断开下游、
    # 并且把上游一起关掉），不是那个数字。数值一大就变成"和操作系统接收缓冲比大小"，
    # 在 Windows 上接收缓冲会自动涨，压不出稳定结果（试过 200 KB：客户端一点没读，
    # 桥的积压却始终顶不到，检查空转）。阈值小，触发就是确定的。
    # 五个口，每个的用途都钉死在一段预留里（见 free_port_block 与 Firehose 的注释：
    # 让内核临时分号的写法在这台机器上已经骗过两次）。
    #   base   桥的命令监听        base+1  桥的观测监听
    #   base+2 没人听的命令上游     base+3  firehose（观测上游）
    base = free_port_block(4)
    with Firehose(port=base + 3) as firehose:
        with Bridge(base + 2, base, base + 1,
                    max_buffered=10_000, obs_upstream_port=firehose.port):
            # **先把接收缓冲调小，再连。** 默认接收缓冲会自动涨到几 MB，那期间字节
            # 全堆在操作系统的缓冲里，根本轮不到桥去积压——上限永远碰不到，检查就
            # 成了空转（第一版正是这么假过的）。压到 8 KB，积压才真的发生在桥里。
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
            sock.settimeout(5.0)
            sock.connect(("127.0.0.1", base + 1))
            try:
                # 一点都不读，等桥自己把上限顶破。
                time.sleep(1.5)
                want(firehose.connected, "上游没被连上，检查没开始")

                deadline = time.monotonic() + 20.0
                closed = False
                while time.monotonic() < deadline:
                    try:
                        chunk = sock.recv(65536)
                    except socket.timeout:
                        break
                    except (ConnectionResetError, OSError):
                        closed = True
                        break
                    if not chunk:
                        closed = True
                        break
                want(closed, "只连不读的客户端没有被桥断开（积压无上限）")
                want(firehose.peer_went_away,
                     "上游没被桥关掉——那断开就不是桥的积压上限做的")
            finally:
                sock.close()


CHECKS = [
    ("观测口：满窗口 DUMP 跨桥搬运后仍自洽", check_observation_pump),
    ("命令口：双前缀与命令计数跨桥不变", check_command_pump),
    ("命令口：第二个客户端被明确拒绝，且不影响第一个", check_command_port_rejects_second_client),
    ("观测口：换手顶替且字节流相位归零", check_observation_replaces_and_resyncs),
    ("上游断开时下游跟着断（不假装还活着）", check_upstream_loss_closes_downstream),
    ("只连不读的客户端被桥的上限断开（不是主站先断）",
     check_stalled_client_is_disconnected),
]

NOT_COVERED = [
    "上游是 AF_UNIX 的那条路：本机（Windows）没有 AF_UNIX，只能用 TCP 上游测。"
    "台架脚本 bench_gui_e2e.sh 里跑的就是 unix 上游，那一次才算覆盖到。",
    "CPU 亲和：本机没有 sched_setaffinity，也不会有 /proc 里的主站线程，"
    "整条路走的是「不支持」分支。台架上要看启动那行日志以及实际掩码。",
    "跨机器（真正的局域网）：本机两个口都在 127.0.0.1，没验过经网卡的吞吐与延迟。",
    "换手瞬间**正卡在上游套接字里**的那几个字节：造不稳定。桥是主动抽干上游的，"
    "积压基本都落在自己的待发缓冲里，而那个在换手时是直接丢掉的——所以这条竞态"
    "在本机压不出来。设计上仍然在换手时连上游一起换，把那几个字节关掉；"
    "但**没有一个会失败的检查**钉住它，只有代码注释和这条记录。",
]


def main():
    failures = []
    for name, function in CHECKS:
        started = time.monotonic()
        try:
            function({})
        except Exception as error:  # noqa: BLE001
            print(f"FAIL  {name}  ({time.monotonic() - started:.2f}s)")
            print(f"      {type(error).__name__}: {error}")
            failures.append(name)
        else:
            print(f"ok    {name}  ({time.monotonic() - started:.2f}s)")

    print()
    print(f"{len(CHECKS) - len(failures)}/{len(CHECKS)} 项通过")
    if failures:
        print("失败：")
        for name in failures:
            print(f"  - {name}")
    if failures:
        print()
        print("没证到的：")
        for note in NOT_COVERED:
            print(f"  - {note}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
