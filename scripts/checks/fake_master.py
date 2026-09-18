#!/usr/bin/env python3
"""离线假主站：真的开两个 TCP 端口，按主站的线协议应答。

**和 console_engine_check.py 里那个 FakeMaster 的分工。** 那个是内存替身（进程内、
command() 直接返回字符串），跑得快，用来测引擎与按键逻辑；这个是进程级服务器，
走真 socket，用来测"跨进程那一层"——客户端库、转发桥、图形界面。本机（Windows）
Python 没有 AF_UNIX，而主站套接字只有 AF_UNIX，所以离线测这一层只能用 TCP；
这也正是 tools/socket_bridge.py 存在的理由。

**复刻的是真主站的行为，不是它的实现。** 刻意保真的六条（每条都是踩过的）：

1. **一次 read 就是一条命令。** 主站不按行累积（command_server.c:176,198-276），
   两条命令挤进一次 read 时后面那条被静默丢弃。这里照做，并把丢掉的那几条记进
   counters.dropped_tail——测试拿它断言"客户端没有 pipeline"。这是唯一能证伪
   "客户端会不会图快把两条命令塞进一个包"的办法。
2. **单步超限先回 OK，然后打掉整个会话。** 真主站里那条 set_external_target 已经
   回成功了，判 MOTION_INVALID 与中止会话发生在其后的周期线程里
   （session_target.c:97-111 → note_runtime_failure）。这条是有意保真的：内存替身
   在这条上比真主站**宽容**（它直接回 ERROR），而宽容的替身测不出真问题。
3. **双前缀。** set_external_target 的回复线上是 "OK|OK|Updated N external targets"
   （session_control.c:1028 写一遍，command_server.c:528 再包一层）。不照抄它，
   客户端那套"只按 OK/ERROR 开头判"的写法就测不到。
4. **写回复用非阻塞**，短写/EAGAIN 一律当"客户端跟不上"，直接断开
   （command_server.c:553-566）。所以"连上就不读"的客户端会被踢，而不是把主站拖住。
5. **单客户端。** 第二个客户端不被 accept（在内核层 connect 可能成功，但永远拿不到
   回复，表现为挂住），不是收到一条拒绝。命令口与观测口各自如此，观测口是
   "新连接顶掉旧的"（server.c:489）。
6. **空闲超时。** 命令口默认 5 秒（command_server.c:96）。

观测口按 include/emaster/observation/*.h 的线格式造帧：256 槽的环、publish_index
连续递增、cycle 会跳号、以及可注入的坏帧（跳号 / 错过截止期 / WKC 不符 / 缺速度力矩）。
struct 布局在这里**独立写一遍**，不从 tools/observation_client.py 抄——抄了就变成
自证，客户端布局漂了也看不出来。

用法（跑起来给人看）：
    python scripts/checks/fake_master.py --port 5101
    # 命令口 5101、观测口 5102；Ctrl-C 停，退出时打一份计数摘要

用法（测试里当库用）：
    from fake_master import FakeMaster
    master = FakeMaster(port=0)        # 端口 0 = 让内核挑
    master.start()
    ... master.counters["aborts"] ...
    master.stop()
"""

import argparse
import os
import select
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "tools"))

# ---------------------------------------------------------------- 线格式
# 与 include/emaster/observation/wire.h 的偏移表逐条对应，独立声明（见模块开头）。
WIRE_VERSION = 1
WIRE_MAGIC = b"EO"
WIRE_KIND_FRAME = 1
WIRE_KIND_DUMP = 2
HEADER_BYTES = 56
AXIS_BYTES = 24
DUMP_HEADER_BYTES = 24
RING_CAPACITY = 256
MAX_AXES = 16

# magic version kind frame_bytes axis_count flags wkc
# publish_index cycle mono_ns deadline_ns interval_ns
_HEADER = struct.Struct("<2sBBHHIiQQQQQ")
# pos target vel torque status control flags
_AXIS = struct.Struct("<iiiiHHI")
# magic version kind header_bytes axis_count first_index frame_count frame_bytes
_DUMP_HEADER = struct.Struct("<2sBBHHQII")

assert _HEADER.size == HEADER_BYTES
assert _AXIS.size == AXIS_BYTES
assert _DUMP_HEADER.size == DUMP_HEADER_BYTES

# 帧级标志（frame.h:69-80）
FRAME_FLAG_WKC_MISMATCH = 1 << 0
FRAME_FLAG_WHOLE_FRAME_MISSING = 1 << 1
FRAME_FLAG_DEADLINE_MISSED = 1 << 2
FRAME_FLAG_CYCLE_GAP = 1 << 3

# 轴级标志（frame.h:40-66）
AXIS_FLAG_WKC_MISMATCH = 1 << 0
AXIS_FLAG_WHOLE_FRAME_MISSING = 1 << 1
AXIS_FLAG_DECODE_INCOMPLETE = 1 << 2
AXIS_FLAG_ISOLATED = 1 << 3
AXIS_FLAG_TARGET_UNKNOWN = 1 << 4
AXIS_FLAG_VELOCITY_UNAVAILABLE = 1 << 5
AXIS_FLAG_TORQUE_UNAVAILABLE = 1 << 6

# 命令口常量，与 command_server.c 对齐。
COMMAND_READ_MAX = 511              # 一次 read 的上限（buffer 512）
COMMAND_CLIENT_IDLE_TIMEOUT_S = 5.0  # command_server.c:96

DEFAULT_AXES = 5
DEFAULT_MAX_STEP = 6400             # counts，与 orangepi-bench-quint 部署同量级
DEFAULT_ENCODER = 16384
DEFAULT_GEAR = "28/1"

# 主站状态机的整数（docs/protocol/command-protocol-v1.md:94-101）
STATE_INITIALIZING = 0
STATE_RUNNING = 4


class _Axis:
    __slots__ = ("planned", "actual", "torque", "velocity")

    def __init__(self):
        self.planned = 0
        self.actual = 0.0
        self.torque = 0
        self.velocity = 0


class FakeMaster:
    """一个进程里的假主站：命令口 + 观测口 + 一个发布线程。

    `counters` 是给测试看的（真主站不提供这些），刻意与线协议分开——它们是
    "这个假主站都经历了什么"，不是协议的一部分。
    """

    def __init__(self, host="127.0.0.1", port=0, obs_port=None, axes=DEFAULT_AXES,
                 max_step=DEFAULT_MAX_STEP, publish_hz=200.0, ready_after=0.0,
                 session_aborts_on_step=True,
                 idle_timeout=COMMAND_CLIENT_IDLE_TIMEOUT_S):
        self.host = host
        self.axes_count = axes
        self.max_step = max_step
        self.publish_hz = publish_hz
        self.session_aborts_on_step = session_aborts_on_step
        self.idle_timeout = idle_timeout

        self.lock = threading.Lock()
        self.state = STATE_INITIALIZING if ready_after > 0 else STATE_RUNNING
        self.cycle = 0
        self.axes = [_Axis() for _ in range(axes)]
        self.halted = False
        self.aborts = []

        self.stats_lock = threading.Lock()
        self.counters = {
            "commands": 0,          # 被当命令处理的 read 次数
            "dropped_tail": 0,      # 同一次 read 里跟在第一条后面的（真主站静默丢弃）
            "step_violations": 0,
            "aborts": 0,
            "rejects_not_running": 0,
            "unknown_commands": 0,
            "clients_kicked_unread": 0,
            "obs_accepts": 0,          # 观测口一共 accept 过几次
            "clients_replaced_obs": 0,  # 其中"顶掉了上一个"的次数
            "published": 0,
            "frames_overwritten": 0,
        }

        # 环：只存字段，按 DUMP（二进制）或 LATEST（文本）现编。
        self.ring_lock = threading.Lock()
        self.ring = [None] * RING_CAPACITY
        self.head = 0               # 开区间上界，与 ring.c 的 head 同义

        # 注入选项（测试用）
        self.inject_cycle_gap_every = 0
        self.inject_deadline_every = 0
        self.inject_wkc_every = 0
        self.inject_velocity_unavailable = False
        self.inject_torque_unavailable = False

        self.running = False
        self._threads = []
        self._ready_at = time.monotonic() + ready_after
        self.cmd_listener = None
        self.obs_listener = None
        self.port = port
        self.obs_port = obs_port
        # 已 accept 的连接。stop() 要把它们也掐掉，否则线程会卡在 recv 上直到超时——
        # 测试里 start/stop 是常事，几秒的尾巴会累积成很烦的等待。
        self._open_conns = []

    # ---------- 生命周期 ----------

    def start(self):
        # 幂等：`with FakeMaster().start()` 这种写法会走两次 start（显式一次、
        # __enter__ 再一次），不挡住就会起两套线程共用一份状态——两条发布循环、
        # 两个 accept 循环，症状是"客户端刚连上就被复位"，看着像网络问题。
        if self.running:
            return self
        self.running = True
        self.cmd_listener = self._listen(self.port)
        self.port = self.cmd_listener.getsockname()[1]
        # "+1" 这条约定只对**人给的**端口成立（与桥的 --port/--obs-port 同一条规矩）。
        # 端口是内核挑的（port=0，测试里就是这样）时不能这么推：那时 port+1 是另一个
        # 内核已经分配出去的号，撞上别的监听口会让客户端连到错误的进程上，而且因为
        # SO_REUSEADDR 在 Windows 上允许抢占，绑定时一声不响。
        want_obs = self.obs_port if self.obs_port else (self.port + 1 if self.port else 0)
        self.obs_listener = self._listen(want_obs)
        self.obs_port = self.obs_listener.getsockname()[1]

        self._spawn(self._command_loop)
        self._spawn(self._observation_loop)
        self._spawn(self._publish_loop)
        return self

    def stop(self):
        self.running = False
        for listener in (self.cmd_listener, self.obs_listener):
            if listener is not None:
                try:
                    listener.close()
                except OSError:
                    pass
        for conn in list(self._open_conns):
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass
        for thread in self._threads:
            thread.join(timeout=2.0)
        self._threads = []
        self._open_conns = []
        return self

    def __enter__(self):
        # start() 自己幂等，这里不必再判一次。
        return self.start()

    def __exit__(self, *_):
        self.stop()

    def _spawn(self, target):
        thread = threading.Thread(target=target, daemon=True)
        thread.start()
        self._threads.append(thread)
        return thread

    def _listen(self, port):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((self.host, port))
        # 与真主站一样只留一格：第二个客户端连得上却永远不会被 accept。
        listener.listen(1)
        listener.settimeout(0.2)
        return listener

    # ---------- 计数 ----------

    def bump(self, key, amount=1):
        with self.stats_lock:
            self.counters[key] += amount

    def snapshot_counters(self):
        with self.stats_lock:
            return dict(self.counters)

    # ---------- 运动模型 ----------

    def _advance(self, dt):
        """把实际位置朝目标挪一点，留出跟随误差（曲线才有东西可看）。"""
        for axis in self.axes:
            delta = axis.planned - axis.actual
            if abs(delta) < 0.5:
                axis.actual = float(axis.planned)
                axis.velocity = 0
            else:
                step = max(-8000.0, min(8000.0, delta * 0.25))
                axis.actual += step
                axis.velocity = int(step / max(dt, 1e-6))

    # ---------- 命令口 ----------

    def _command_loop(self):
        while self.running:
            try:
                conn, _ = self.cmd_listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self._serve_command_client(conn)
            finally:
                self._close(conn)

    def _serve_command_client(self, conn):
        conn.setblocking(False)
        self._open_conns.append(conn)
        idle_deadline = time.monotonic() + self.idle_timeout
        while self.running:
            # 分小片等，而不是一次等满 5 秒：等满的话 stop() 得陪着等 5 秒才收得回线程。
            try:
                ready, _, _ = select.select([conn], [], [], 0.2)
            except (OSError, ValueError):
                return
            if not ready:
                if time.monotonic() >= idle_deadline:
                    return          # 空闲超时：单方面关连接（真主站也是这样）
                continue
            try:
                chunk = conn.recv(COMMAND_READ_MAX)
            except BlockingIOError:
                continue
            except OSError:
                return
            if not chunk:
                return              # 对端关了
            idle_deadline = time.monotonic() + self.idle_timeout

            text = chunk.decode("utf-8", errors="replace")
            newline = text.find("\n")
            if newline >= 0:
                tail = text[newline + 1:]
                # 真主站对后面几条是静默丢弃的。这里记一笔，好让测试断言客户端
                # 从来没这么发过——丢掉的条数不为 0 就意味着有一次真的丢了。
                extra = [line for line in tail.split("\n") if line.strip()]
                if extra:
                    self.bump("dropped_tail", len(extra))
                text = text[:newline]

            self.bump("commands")
            reply = self._handle(text.strip())
            if not self._write(conn, reply + "\n"):
                self.bump("clients_kicked_unread")
                return

    @staticmethod
    def _write(conn, payload):
        """非阻塞写。短写/EAGAIN 一律当客户端跟不上——真主站直接断开。"""
        data = payload.encode("utf-8")
        sent = 0
        while sent < len(data):
            try:
                count = conn.send(data[sent:])
            except BlockingIOError:
                return False
            except OSError:
                return False
            if count <= 0:
                return False
            sent += count
        return True

    def _handle(self, line):
        with self.lock:
            now = time.monotonic()
            dt = max(now - getattr(self, "_last_advance", now), 1e-6)
            self._last_advance = now
            if self.state == STATE_RUNNING and not self.halted:
                self._advance(dt * 4.0)
                self.cycle += 1

            parts = line.split()
            if not parts:
                return "ERROR|empty command"
            name = parts[0]

            if name == "status":
                return self._reply_status()

            if name == "topology":
                body = f"OK|axes={self.axes_count},max_step={self.max_step}"
                for index in range(self.axes_count):
                    body += (f"|a{index + 1}:bus={index + 1},enc={DEFAULT_ENCODER},"
                             f"gear={DEFAULT_GEAR},torque=0")
                return body

            if name == "halt":
                self.halted = len(parts) > 1 and parts[1] == "1"
                return (f"OK|halt {'set' if self.halted else 'cleared'} "
                        f"for {self.axes_count} axes")

            if name == "fault_reset":
                return f"OK|fault reset requested for {self.axes_count} axes"

            if name == "quick_stop":
                return f"OK|quick stop requested for {self.axes_count} axes"

            if name in ("stop", "shutdown"):
                # 真主站：只置 stop_requested，健康会话里安全门恒 false，
                # 所以**什么都不会发生**。这里如实复刻这个空操作。
                return "OK|stop requested"

            if name == "set_external_target":
                return self._handle_target(parts)

            self.bump("unknown_commands")
            return "ERROR|unknown command type"

    def _reply_status(self):
        word = 0x0027           # Operation Enabled
        body = (f"OK|state={self.state} cycle={self.cycle} axes={self.axes_count} "
                f"enabled=1 completed=0")
        for index, axis in enumerate(self.axes):
            # halt 只是控制字 bit8，不改变 CiA402 状态机，所以状态字仍是使能。
            axis_word = word
            if abs(axis.actual - axis.planned) < 2:
                axis_word |= 0x0400         # 到位
            body += (f"|a{index + 1}:pos={int(axis.actual)},vel={axis.velocity},"
                     f"torque={axis.torque},status=0x{axis_word:04x},"
                     f"target_pos={int(axis.actual)},planned={axis.planned},"
                     f"err=0x0000,state=6,mode=8")
        return body

    def _handle_target(self, parts):
        """set_external_target。语义上最要紧的一条在这里（见模块开头第 2 条）。"""
        if self.state != STATE_RUNNING:
            self.bump("rejects_not_running")
            return f"ERROR|ERROR|Not ready: state={self.state} (need RUNNING=4)"

        values = parts[1:]
        if len(values) != self.axes_count:
            return (f"ERROR|ERROR|Wrong count: expected {self.axes_count}, "
                    f"got {len(values)}")
        try:
            targets = [int(value) for value in values]
        except ValueError:
            return "ERROR|ERROR|Invalid position value"

        deltas = [abs(target - axis.planned)
                  for target, axis in zip(targets, self.axes)]
        largest = max(deltas) if deltas else 0
        if largest > self.max_step and self.session_aborts_on_step:
            # 保真点：**先**照常接受（真主站那条命令确实回了 OK），**再**中止会话。
            for target, axis in zip(targets, self.axes):
                axis.planned = target
            self.bump("step_violations")
            self.bump("aborts")
            self.aborts.append(largest)
            self.state = STATE_INITIALIZING      # 整个会话没了
            return f"OK|OK|Updated {len(targets)} external targets"

        for target, axis in zip(targets, self.axes):
            axis.planned = target
        return f"OK|OK|Updated {len(targets)} external targets"

    # ---------- 观测口 ----------

    def _publish_loop(self):
        period = 1.0 / self.publish_hz if self.publish_hz > 0 else 0.005
        next_at = time.monotonic()
        stride = 1
        published = 0
        while self.running:
            next_at += period
            sleep_for = next_at - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            else:
                next_at = time.monotonic()     # 落后了就重新对齐，别攒追赶

            if time.monotonic() < self._ready_at:
                continue

            published += 1
            with self.lock:
                self.cycle += 1
                cycle = self.cycle
                axes_snapshot = [(int(a.actual), a.planned, a.velocity, a.torque)
                                 for a in self.axes]
                state = self.state

            flags = 0
            axis_flags = 0
            wkc = self.axes_count
            if self.inject_cycle_gap_every and published % self.inject_cycle_gap_every == 0:
                # 主站跳拍：cycle 多走几格，publish_index 仍然连续。
                cycle += 2 * stride
                flags |= FRAME_FLAG_CYCLE_GAP
            if self.inject_deadline_every and published % self.inject_deadline_every == 0:
                flags |= FRAME_FLAG_DEADLINE_MISSED
            if self.inject_wkc_every and published % self.inject_wkc_every == 0:
                flags |= FRAME_FLAG_WKC_MISMATCH
                axis_flags |= AXIS_FLAG_WKC_MISMATCH
                wkc -= 1
            if state != STATE_RUNNING:
                flags |= FRAME_FLAG_WHOLE_FRAME_MISSING
                axis_flags |= AXIS_FLAG_WHOLE_FRAME_MISSING
                wkc = 0
            if self.inject_velocity_unavailable:
                axis_flags |= AXIS_FLAG_VELOCITY_UNAVAILABLE
            if self.inject_torque_unavailable:
                axis_flags |= AXIS_FLAG_TORQUE_UNAVAILABLE

            mono_ns = time.monotonic_ns()
            frame = {
                "cycle": cycle,
                "mono_ns": mono_ns,
                "deadline_ns": mono_ns + int(period * 1e9),
                "interval_ns": int(period * 1e9),
                "wkc": wkc,
                "flags": flags,
                "axes": [
                    (pos, target, vel, torque, 0x0027, 0x000F, axis_flags)
                    for (pos, target, vel, torque) in axes_snapshot
                ],
            }
            self._publish(frame)

    def _publish(self, frame):
        with self.ring_lock:
            frame["publish_index"] = self.head
            self.ring[self.head % RING_CAPACITY] = frame
            self.head += 1
            self.bump("published")
            if self.head > RING_CAPACITY:
                self.bump("frames_overwritten")

    def _window(self):
        with self.ring_lock:
            head = self.head
            oldest = (head - RING_CAPACITY + 1) if head >= RING_CAPACITY else 0
            return head, oldest

    def _frame_at(self, index):
        with self.ring_lock:
            if index < 0 or index >= self.head:
                return None
            if index < ((self.head - RING_CAPACITY + 1) if self.head >= RING_CAPACITY else 0):
                return None
            return self.ring[index % RING_CAPACITY]

    def _encode_frame(self, frame):
        axes = frame["axes"][:MAX_AXES]
        return _HEADER.pack(
            WIRE_MAGIC, WIRE_VERSION, WIRE_KIND_FRAME,
            HEADER_BYTES + AXIS_BYTES * len(axes), len(axes),
            frame["flags"], frame["wkc"], frame["publish_index"], frame["cycle"],
            frame["mono_ns"], frame["deadline_ns"], frame["interval_ns"],
        ) + b"".join(_AXIS.pack(*axis) for axis in axes)

    def _observation_loop(self):
        """观测口：单客户端，**新连接顶掉旧的**（server.c:489）。

        用 select 同时看着监听口与当前那条连接。此前的写法是 accept 完就一头扎进
        服务循环，于是第二个客户端只会堵在 backlog 里等第一个走——那是命令口的
        语义（第二个永远不被 accept），不是观测口的。
        """
        active = None
        pending = bytearray()
        while self.running:
            watchers = [self.obs_listener]
            if active is not None:
                watchers.append(active)
            try:
                ready, _, _ = select.select(watchers, [], [], 0.2)
            except (OSError, ValueError):
                break

            if self.obs_listener in ready:
                try:
                    conn, _ = self.obs_listener.accept()
                except (socket.timeout, BlockingIOError):
                    conn = None
                except OSError:
                    conn = None
                if conn is not None:
                    self.bump("obs_accepts")
                    if active is not None:
                        self.bump("clients_replaced_obs")
                        self._close(active)
                    conn.setblocking(False)
                    self._open_conns.append(conn)
                    active = conn
                    pending = bytearray()

            if active is None or active not in ready:
                continue

            try:
                chunk = active.recv(4096)
            except (BlockingIOError, socket.timeout):
                continue
            except OSError:
                chunk = b""
            if not chunk:
                self._close(active)
                active = None
                pending = bytearray()
                continue

            pending += chunk
            while b"\n" in pending:
                line, _, rest = bytes(pending).partition(b"\n")
                pending = bytearray(rest)
                reply = self._handle_verb(line.decode("ascii", "replace").strip())
                if reply is None:
                    continue
                try:
                    active.sendall(reply)
                except (BlockingIOError, OSError):
                    # 非阻塞写没能一次写完 = 客户端跟不上。真主站对短写的处置是断开
                    # （server.c 的背压路径），而不是把半条回复留在流里——留半条会
                    # 让对端按错位的字节解，比断开危险得多。
                    self.bump("clients_kicked_unread")
                    self._close(active)
                    active = None
                    pending = bytearray()
                    break

        if active is not None:
            self._close(active)

    def _close(self, conn):
        try:
            conn.close()
        except OSError:
            pass
        try:
            self._open_conns.remove(conn)
        except ValueError:
            pass

    def _handle_verb(self, verb):
        """观测口的四个动词。返回要发回去的字节，None 表示不发。"""
        if verb == "INFO":
            line = (f"OK|wire_version={WIRE_VERSION}|deployment=fake-bench"
                    f"|interface=fake0|axes={self.axes_count}|stride=1"
                    f"|capacity={RING_CAPACITY}"
                    f"|frame_bytes={HEADER_BYTES + AXIS_BYTES * self.axes_count}"
                    f"|axis_bytes={AXIS_BYTES}|verbs=INFO,HEAD,LATEST,DUMP\n")
            return line.encode("ascii")

        head, oldest = self._window()
        if verb == "HEAD":
            if head == 0:
                return f"OK|head=0|empty=1|capacity={RING_CAPACITY}\n".encode("ascii")
            return (f"OK|oldest={oldest}|newest={head}|capacity={RING_CAPACITY}\n"
                    ).encode("ascii")

        if verb == "LATEST":
            if head == 0:
                return b"OK|empty=1\n"
            frame = self._frame_at(head - 1)
            if frame is None:
                return b"ERROR|unstable\n"
            text = (f"OK|publish_index={frame['publish_index']} cycle={frame['cycle']}"
                    f" mono_ns={frame['mono_ns']} interval_ns={frame['interval_ns']}"
                    f" wkc={frame['wkc']} axes={len(frame['axes'])}"
                    f" flags=0x{frame['flags']:08x}")
            for index, (pos, target, vel, torque, status, control,
                        axis_flags) in enumerate(frame["axes"]):
                text += (f"|a{index + 1}:pos={pos},vel={vel},torque={torque},"
                         f"status=0x{status:04x},target_pos={target},"
                         f"control=0x{control:04x},flags=0x{axis_flags:08x}")
            return (text + "\n").encode("ascii")

        if verb.startswith("DUMP"):
            parts = verb.split()
            try:
                start = int(parts[1]) if len(parts) > 1 else 0
                count = int(parts[2]) if len(parts) > 2 else 1
            except ValueError:
                return b"ERROR|bad dump range\n"
            count = max(0, min(count, RING_CAPACITY))
            if head == 0:
                start = 0
            elif start < oldest:
                start = oldest
            frames = []
            index = start
            # 在第一个空洞处停下——与 server.c:336-342 同一条规矩。
            while index < head and len(frames) < count:
                frame = self._frame_at(index)
                if frame is None:
                    break
                frames.append(frame)
                index += 1
            encoded = [self._encode_frame(frame) for frame in frames]
            axis_count = len(frames[0]["axes"]) if frames else 0
            header = _DUMP_HEADER.pack(
                WIRE_MAGIC, WIRE_VERSION, WIRE_KIND_DUMP, DUMP_HEADER_BYTES,
                axis_count, start, len(encoded),
                HEADER_BYTES + AXIS_BYTES * axis_count,
            )
            return header + b"".join(encoded)

        return None


def _main():
    ap = argparse.ArgumentParser(description="离线假主站（命令口 + 观测口，TCP）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5101, help="命令口（0 = 内核挑）")
    ap.add_argument("--obs-port", type=int, default=None, help="观测口（默认命令口 +1）")
    ap.add_argument("--axes", type=int, default=DEFAULT_AXES)
    ap.add_argument("--publish-hz", type=float, default=200.0)
    ap.add_argument("--ready-after", type=float, default=0.0,
                    help="这么多秒之后才进 RUNNING（用来测客户端的等待路径）")
    ap.add_argument("--idle-timeout", type=float, default=COMMAND_CLIENT_IDLE_TIMEOUT_S,
                    help=f"命令口空闲断开秒数（真主站是 {COMMAND_CLIENT_IDLE_TIMEOUT_S}）")
    args = ap.parse_args()

    master = FakeMaster(host=args.host, port=args.port, obs_port=args.obs_port,
                        axes=args.axes, publish_hz=args.publish_hz,
                        ready_after=args.ready_after,
                        idle_timeout=args.idle_timeout).start()
    print(f"假主站就绪：命令口 {master.host}:{master.port}，"
          f"观测口 {master.host}:{master.obs_port}，{args.axes} 轴", flush=True)
    print("Ctrl-C 停，退出时打一份计数摘要", flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        master.stop()
    for key, value in sorted(master.snapshot_counters().items()):
        print(f"  {key:26s} {value}")
    if master.aborts:
        print(f"  单步违规的增量            {master.aborts}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
