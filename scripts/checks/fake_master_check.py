#!/usr/bin/env python3
"""验证假主站本身**是不是忠实的**。

为什么值得单独一个检查：接下来的界面回归会拿这个假主站当唯一对手。假主站要是
在某个行为上比真主站宽容，界面就会在那个行为上"通过一个不存在的测试"——而它掩盖
的恰恰是台架上才会发作的东西。所以这里逐条钉的是**刻意的保真点**，每条都对应真
主站的一处实现：

- 单步超限**先回 OK 再打掉会话**（session_target.c:97-111 → note_runtime_failure）
- 回复的**双前缀**（session_control.c:1028 + command_server.c:528）
- **一次 read 一条命令**，多出来的静默丢弃（command_server.c:176,198-276）
- **空闲断开**（command_server.c:96）
- 观测口**新连接顶掉旧的**（server.c:489）
- HEAD 的 `newest` 是**开区间上界**（ring.c:155）
- DUMP 在**第一个空洞处停下**并如实报 frame_count（server.c:336-342）

没证到的写在 --verbose 的输出末尾（"没证到的"一节），不含糊过去。

跑法：python scripts/checks/fake_master_check.py [--verbose]
"""

import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fake_master  # noqa: E402
import emaster_client  # noqa: E402
import observation_client as obs_wire  # noqa: E402


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


# ---------------------------------------------------------------- 命令口

def _cmd_connect(master):
    sock = socket.create_connection(("127.0.0.1", master.port), 5.0)
    sock.settimeout(5.0)
    return sock


def _ask(sock, command):
    """发一条、读一整行。**绝不 pipeline**——本条通道一次 read 只当一条命令。"""
    sock.sendall((command + "\n").encode())
    data = bytearray()
    while True:
        byte = sock.recv(1)
        if not byte:
            raise CheckFailed(f"{command!r} 的回复中途断了（收到 {bytes(data)!r}）")
        if byte == b"\n":
            return data.decode("utf-8", "replace")
        data += byte


def check_cmd_basic(context):
    with fake_master.FakeMaster(publish_hz=200.0) as master:
        sock = _cmd_connect(master)
        try:
            status = _ask(sock, "status")
            want(emaster_client.reply_kind(status) == "ok", f"status 不是 OK：{status!r}")
            want("state=4" in status, f"开局应当 RUNNING：{status!r}")
            want(f"axes={fake_master.DEFAULT_AXES}" in status,
                 f"轴数不对：{status!r}")
            want(status.count("|a") == fake_master.DEFAULT_AXES,
                 f"轴行数不等于轴数：{status!r}")

            topology = _ask(sock, "topology")
            want(f"max_step={fake_master.DEFAULT_MAX_STEP}" in topology,
                 f"topology 没带 max_step：{topology!r}")

            unknown = _ask(sock, "no_such_verb")
            want(emaster_client.reply_kind(unknown) == "error",
                 f"未知动词应当报错：{unknown!r}")
        finally:
            sock.close()


def check_double_prefix(context):
    """双前缀必须真的在线上，否则界面那套"只按开头判"的写法等于没测。"""
    with fake_master.FakeMaster() as master:
        sock = _cmd_connect(master)
        try:
            reply = _ask(sock, "set_external_target 0 0 0 0 0")
            want(reply.startswith("OK|OK|"),
                 f"set_external_target 应当是双前缀，实际 {reply!r}")
            want(emaster_client.reply_kind(reply) == "ok",
                 "双前缀被 reply_kind 判错了")
            text = emaster_client.reply_text(reply)
            want(text.startswith("Updated"),
                 f"reply_text 没剥干净：{text!r}")
            want("OK" not in text and "ERROR" not in text,
                 f"reply_text 留下了前缀：{text!r}")
        finally:
            sock.close()


def check_step_violation_is_faithful(context):
    """本检查的重点：超限那条命令**回 OK**，然后会话才没。

    内存替身（console_engine_check.FakeMaster）在这一点上直接回 ERROR，比真主站
    宽容。宽容的替身测不出"客户端按 OK 继续推进、底下会话已经没了"这个序列。
    """
    with fake_master.FakeMaster(max_step=6400) as master:
        sock = _cmd_connect(master)
        try:
            reply = _ask(sock, f"set_external_target {6400 * 4} 0 0 0 0")
            want(emaster_client.reply_kind(reply) == "ok",
                 f"超限那条命令真主站回的是 OK，这里却是 {reply!r}")
            status = _ask(sock, "status")
            want("state=0" in status,
                 f"超限之后会话应当被打掉（state=0），实际 {status!r}")
            again = _ask(sock, "set_external_target 0 0 0 0 0")
            want(emaster_client.reply_kind(again) == "error",
                 f"会话没了还接受目标：{again!r}")
            counters = master.snapshot_counters()
            want(counters["aborts"] == 1, f"中止计数不对：{counters}")
            want(counters["step_violations"] == 1, f"违规计数不对：{counters}")
            want(counters["rejects_not_running"] == 1, f"拒绝计数不对：{counters}")
        finally:
            sock.close()


def check_no_pipelining(context):
    """两条命令挤进一次 write：真主站静默丢弃后一条。这里要能**看出来**丢了。

    这条计数器就是"客户端有没有图快"的唯一证据——没有它，客户端 pipeline 了也
    只是表现为偶尔少一条回复，很难归因。
    """
    # 正型：规规矩矩一次一条。计数器必须停在 0——否则下面那条断言就没有意义了。
    with fake_master.FakeMaster() as master:
        sock = _cmd_connect(master)
        try:
            for _ in range(3):
                want(emaster_client.reply_kind(_ask(sock, "status")) == "ok",
                     "一条一条发也该正常")
            counters = master.snapshot_counters()
            want(counters["dropped_tail"] == 0,
                 f"一次一条不该有丢弃：{counters}")
            want(counters["commands"] == 3, f"命令计数不对：{counters}")
        finally:
            sock.close()

    with fake_master.FakeMaster() as master:
        sock = _cmd_connect(master)
        try:
            sock.sendall(b"status\nstatus\n")
            time.sleep(0.3)
            counters = master.snapshot_counters()
            want(counters["dropped_tail"] >= 1,
                 f"同一次 read 里的第二条应当被记成丢弃：{counters}")
            want(counters["commands"] == 1,
                 f"只该当一条命令处理：{counters}")
        finally:
            sock.close()


def check_idle_timeout(context):
    with fake_master.FakeMaster(idle_timeout=0.4) as master:
        sock = _cmd_connect(master)
        try:
            want(emaster_client.reply_kind(_ask(sock, "status")) == "ok",
                 "空闲前应当还能用")
            time.sleep(1.0)
            sock.sendall(b"status\n")
            sock.settimeout(1.0)
            try:
                data = sock.recv(64)
            except (socket.timeout, ConnectionResetError, OSError):
                data = b""
            want(data == b"",
                 f"空闲超时后应当被单方面断开，却收到 {data[:40]!r}")
        finally:
            sock.close()


# ---------------------------------------------------------------- 观测口

def check_obs_info_head_latest(context):
    with fake_master.FakeMaster(publish_hz=200.0) as master:
        client = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            client.connect()
            client.read_info()
            want(client.axis_count == fake_master.DEFAULT_AXES,
                 f"INFO 的轴数不对：{client.info_fields}")
            want(client.ring_capacity == fake_master.RING_CAPACITY,
                 f"INFO 的容量不对：{client.info_fields}")

            # 等它真的发布几帧（ready 之后立刻连的话环可能还是空的）。
            deadline = time.monotonic() + 3.0
            oldest = newest = capacity = 0
            while time.monotonic() < deadline:
                oldest, newest, capacity, empty = client.read_head()
                if not empty and newest - oldest >= 4:
                    break
                time.sleep(0.05)
            want(not empty, "等了 3 秒环还是空的")
            want(capacity == fake_master.RING_CAPACITY,
                 f"HEAD 的容量不对：{capacity}")

            # newest 是开区间上界：可读的是 [oldest, newest)，最后一帧是 newest-1。
            #
            # 但 HEAD 是**会动的**：主站在 200 Hz 上发布，读一次 HEAD 到下一次问
            # LATEST 之间又过去几帧是常态。所以不能拿"读之前的那个 newest"去等号
            # 比较（第一版就是这么写的，跑八轮回挂一次，报 "LATEST 给的是 4，
            # newest=4"）。夹在前后两次 HEAD 之间才是稳的判据。
            before_newest = newest
            frame, axes = client.read_latest()
            want(frame is not None, "LATEST 取不到帧")
            after_oldest, after_newest, _, after_empty = client.read_head()
            want(not after_empty, "HEAD 突然变空了")
            index = frame["publish_index"]
            want(before_newest - 1 <= index <= after_newest - 1,
                 f"LATEST 给的序号 {index} 不在两次 HEAD 之间"
                 f"（{before_newest - 1} .. {after_newest - 1}）："
                 f"newest 是开区间上界这条不成立")
            want(after_oldest <= index, f"LATEST 给的序号 {index} 比可读下界还老")
            want(len(axes) == fake_master.DEFAULT_AXES,
                 f"LATEST 的轴数不对：{len(axes)}")
        finally:
            client.disconnect()


def check_obs_dump_window(context):
    """走真解码器取一段：起点要自洽、publish_index 要连续。"""
    with fake_master.FakeMaster(publish_hz=400.0) as master:
        client = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            client.connect()
            client.read_info()
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                oldest, newest, _, empty = client.read_head()
                if not empty and newest - oldest >= 64:
                    break
                time.sleep(0.05)
            want(newest - oldest >= 64, f"环里只有 {newest - oldest} 帧，取不了 64 帧")

            first, frames = client.read_window(newest - 64, 64)
            want(len(frames) == 64, f"应当取到 64 帧，实际 {len(frames)}")
            indices = [f["publish_index"] for f in frames]
            want(indices[0] == first,
                 f"事务头起点 {first} 与首帧序号 {indices[0]} 不一致")
            holes = sum(1 for a, b in zip(indices, indices[1:]) if b != a + 1)
            want(holes == 0, f"窗口里有 {holes} 处跳号（不该有）")
            axis_frames = frames[0]["axes"]
            want(len(axis_frames) == fake_master.DEFAULT_AXES,
                 f"帧里的轴数不对：{len(axis_frames)}")
        finally:
            client.disconnect()


def check_obs_ring_overwrite(context):
    """发布数超过环容量之后，可读下界要跟着上移——这正是"取数跟不上就永久丢"。"""
    with fake_master.FakeMaster(publish_hz=1000.0) as master:
        client = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            client.connect()
            client.read_info()
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                oldest, newest, _, empty = client.read_head()
                if not empty and newest > fake_master.RING_CAPACITY + 16:
                    break
                time.sleep(0.05)
            want(newest > fake_master.RING_CAPACITY,
                 f"5 秒内没发满一环（newest={newest}）")
            # 下界是 head - CAPACITY + 1，**不是** head - CAPACITY：head % CAPACITY
            # 那个槽正要被写，是 hazard 槽，必须排除在外（ring.c:34-44，那段注释说
            # 这条边界是自测在 head=CAP 上抓出来的）。所以 256 槽的环实际可读 255 帧，
            # 可读区间是 [oldest, newest)，不是 [oldest, newest]。
            want(oldest == newest - fake_master.RING_CAPACITY + 1,
                 f"环满之后可读下界应当是 newest-CAPACITY+1："
                 f"oldest={oldest} newest={newest}")
            want(newest - oldest == fake_master.RING_CAPACITY - 1,
                 f"环满之后可读帧数应当是容量减一（hazard 槽除外）："
                 f"{newest - oldest}")
            # 请求一个早就被覆盖掉的起点：服务端要把它钳到可读下界。
            #
            # 钳到的是**发起那一刻**的下界，不是上面读到的那一个——1000 Hz 下
            # 两次调用之间又发了几帧，下界跟着上移。第一版拿上面的 oldest 做等号
            # 比较，跑八轮回挂一次（"应当被钳到 61，实际 62"）。夹在前后两次 HEAD
            # 之间即可，同时 first > 0 才是"确实钳了"而不是"照单全收"。
            oldest_before = oldest
            first, frames = client.read_window(0, 8)
            oldest_after, _, _, _ = client.read_head()
            want(first > 0, "请求第 0 帧却被原样受理了，没有钳")
            want(oldest_before <= first <= oldest_after,
                 f"越界起点应当被钳到当时的可读下界（{oldest_before} .. "
                 f"{oldest_after} 之间），实际 {first}")
            want(frames, "钳完之后一帧都没有")
        finally:
            client.disconnect()


def check_obs_injections(context):
    """可注入的坏帧：健康面板要能在硬件之外被喂到这些标志。"""
    master = fake_master.FakeMaster(publish_hz=500.0)
    master.inject_cycle_gap_every = 5
    master.inject_deadline_every = 7
    master.inject_wkc_every = 11
    with master.start():
        client = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            client.connect()
            client.read_info()
            deadline = time.monotonic() + 4.0
            while time.monotonic() < deadline:
                _, newest, _, empty = client.read_head()
                if not empty and newest >= 128:
                    break
                time.sleep(0.05)
            first, frames = client.read_window(max(0, newest - 128), 128)
            want(frames, "注入之后一帧都没取到")

            frame_flags = 0
            axis_flags = 0
            for frame in frames:
                frame_flags |= frame["flags"]
                for axis in frame["axes"]:
                    axis_flags |= axis["flags"]
            want(frame_flags & fake_master.FRAME_FLAG_CYCLE_GAP,
                 "注入的 CYCLE_GAP 没出现在帧标志里")
            want(frame_flags & fake_master.FRAME_FLAG_DEADLINE_MISSED,
                 "注入的 DEADLINE_MISSED 没出现在帧标志里")
            want(frame_flags & fake_master.FRAME_FLAG_WKC_MISMATCH,
                 "注入的 WKC_MISMATCH 没出现在帧标志里")
            want(axis_flags & fake_master.AXIS_FLAG_WKC_MISMATCH,
                 "注入的轴级 WKC_MISMATCH 没出现")
            # 跳拍只动 cycle，不动 publish_index：这两件事在真主站里是分开的。
            cycles = [f["cycle"] for f in frames]
            indices = [f["publish_index"] for f in frames]
            want(any(b - a > 1 for a, b in zip(cycles, cycles[1:])),
                 "注入的跳拍没有体现在 cycle 上")
            want(all(b == a + 1 for a, b in zip(indices, indices[1:])),
                 "跳拍不该让 publish_index 跳号")

            names = emaster_client.frame_flags_text(frame_flags)
            want("跳拍" in names and "错过截止期" in names and "WKC不符" in names,
                 f"标志位翻译不全：{names}")
        finally:
            client.disconnect()


def check_obs_unavailable_flags(context):
    """无速度/无力矩反馈：这两个量恒 0，界面必须显示 N/A 而不是 0。"""
    master = fake_master.FakeMaster(publish_hz=500.0)
    master.inject_velocity_unavailable = True
    master.inject_torque_unavailable = True
    with master.start():
        client = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            client.connect()
            client.read_info()
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                _, newest, _, empty = client.read_head()
                if not empty:
                    break
                time.sleep(0.05)
            frame, axes = client.read_latest()
            want(frame is not None, "取不到帧")
            flags = axes[0]["flags"]
            want(flags & emaster_client.AXIS_FLAG_VELOCITY_UNAVAILABLE,
                 "无速度反馈标志没出现")
            want(flags & emaster_client.AXIS_FLAG_TORQUE_UNAVAILABLE,
                 "无力矩反馈标志没出现")
            text = emaster_client.axis_flags_text(flags)
            want("无速度反馈" in text and "无力矩反馈" in text,
                 f"标志位翻译不全：{text}")
            # 未知位不能被吞掉：这是"表和主站不同步"的唯一可见信号。
            bogus = emaster_client.axis_flags_text(1 << 30)
            want("未知位" in bogus, f"未定义的位被吞掉了：{bogus}")
        finally:
            client.disconnect()


def check_obs_single_client_replaced(context):
    """第二个客户端应当**顶掉**第一个，而不是排队等（server.c:489）。"""
    with fake_master.FakeMaster(publish_hz=200.0) as master:
        first = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        second = emaster_client.ObservationClient(f"tcp:127.0.0.1:{master.obs_port}")
        try:
            first.connect()
            first.read_info()
            second.connect()
            second.read_info()
            time.sleep(0.5)

            # 第二个能正常用。
            want(second.read_head()[2] == fake_master.RING_CAPACITY,
                 "第二个客户端连上后不能用")
            # 第一个应当已经被顶掉了：再问一次会拿到 EOF / 复位，而不是回复。
            try:
                first.read_head()
                raise CheckFailed("旧客户端还能用——它应该已经被顶掉")
            except obs_wire.ProtocolError:
                pass
            except OSError:
                pass

            counters = master.snapshot_counters()
            want(counters["clients_replaced_obs"] >= 1,
                 f"顶替计数没涨：{counters}")
        finally:
            first.disconnect()
            second.disconnect()


CHECKS = [
    ("命令口：基本动词与状态形状", check_cmd_basic),
    ("命令口：双前缀 OK|OK|", check_double_prefix),
    ("命令口：单步超限先回 OK 再打掉会话", check_step_violation_is_faithful),
    ("命令口：一次 read 一条命令，多的记成丢弃", check_no_pipelining),
    ("命令口：空闲超时单方面断开", check_idle_timeout),
    ("观测口：INFO/HEAD/LATEST 与 newest 是开区间上界", check_obs_info_head_latest),
    ("观测口：DUMP 窗口自洽且 publish_index 连续", check_obs_dump_window),
    ("观测口：环写满后下界上移、越界起点被钳", check_obs_ring_overwrite),
    ("观测口：注入跳拍/截止期/WKC 都能出现在标志里", check_obs_injections),
    ("观测口：无速度/无力矩反馈的标志与未知位", check_obs_unavailable_flags),
    ("观测口：新连接顶掉旧连接", check_obs_single_client_replaced),
]

NOT_COVERED = [
    "非阻塞短写/写满踢客户端：要先把套接字缓冲填满，本机缓冲太大（几十 KB 起），"
    "造起来不划算；台架上由 observation_client.py --stall 对着真主站测。",
    "命令队列满（队列深 16）的回复：要一次塞 17 条且服务端来不及处理，"
    "非确定性太强，不放进回归。",
]


def main():
    ap = argparse.ArgumentParser(description="验证假主站是否忠实")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    failures = []
    for name, function in CHECKS:
        started = time.monotonic()
        try:
            function({"verbose": args.verbose})
        except Exception as error:  # noqa: BLE001 —— 检查项里什么都可能抛
            elapsed = time.monotonic() - started
            print(f"FAIL  {name}  ({elapsed:.2f}s)")
            print(f"      {type(error).__name__}: {error}")
            failures.append(name)
        else:
            elapsed = time.monotonic() - started
            print(f"ok    {name}  ({elapsed:.2f}s)")

    print()
    print(f"{len(CHECKS) - len(failures)}/{len(CHECKS)} 项通过")
    if failures:
        print("失败：")
        for name in failures:
            print(f"  - {name}")
    if args.verbose or failures:
        print()
        print("没证到的：")
        for note in NOT_COVERED:
            print(f"  - {note}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
