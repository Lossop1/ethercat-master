#!/usr/bin/env python3
"""界面背后的两条 I/O 线程。所有套接字读写都在这里，Qt 主线程一个字节都不碰。

**为什么必须搬出主线程。** 控制拍那条路是阻塞的：Engine.tick() 每叫一次就发一条
命令、等一整行回来。把它放在 Qt 主线程里，主站一卡、或者网络一抖，整个窗口就冻住
——连"主站没回应"这句话都画不出来。观测那条路同理。

**两条通道各自成线程，不合成一条。** 它们连的是主站的两个不同套接字，语义也不同：
命令是"我推它、它应"，观测是"我拉、它给"。合成一条只会让一方的阻塞拖住另一方。

**这里定下的两条节奏，与"别和主站抢资源"这条要求直接相关：**

- 命令口 50 Hz 是**控制拍**（与 TUI 面板、interactive_control 同一档）。它只在
  用户显式"接管"之后才存在；只读模式下这个线程根本不会建。
- 观测口 20 Hz × 64 帧 ≈ 226 KB/s 的取数能力，而主站产出 1 kHz × 单帧约 180 字节
  ≈ 180 KB/s。取数略快于产出，落后了能自己追上；节奏再快也只是空转。

**每个线程各自绑核**（rt_affinity.pin_current_thread），理由见那个模块开头：Linux
上 `sched_setaffinity(0, ...)` 只改调用线程，不是改整个进程。在一个线程里绑一次
然后以为整个界面都躲开了主站，是这条最容易踩空的地方。
"""

import os
import queue
import time

from PyQt5.QtCore import QThread, pyqtSignal

import emaster_client
import emaster_endpoint
import rt_affinity

from emaster_console import (CONTROL_RATE_HZ, DEFAULT_JOG_SPEED, DEFAULT_RANGE_DEG,
                             ConsoleClient, Engine)

# 观测口连不上时的重试间隔。比命令口慢得多——观测口重连只是少几条曲线，
# 而重试太密会把主站那只有一格 accept 的观测口挤爆（server.c 是"新连接顶掉旧的"，
# 密着重连等于反复把自己踢下线）。
OBS_RECONNECT_S = 1.0

# 一次取多少帧。64 帧在 1 kHz 下是 64 ms，比环的 256 ms 小得多，留足了抖动余量。
DEFAULT_OBS_WINDOW = 64


class ObservationPoller(QThread):
    """只读观测。这条通道**没有任何写操作**，连上了也不可能让轴动起来。

    取数是"记住上次取到哪、接着往下取"，而不是每轮都问最新——后者会静默漏帧，
    而且看起来一切正常。接不上的地方（publish_index 跳号）如实记账并在曲线上断开，
    不假装连续。
    """

    frames = pyqtSignal(object)     # {"frames": [...], "oldest":, "newest":, ...}
    link = pyqtSignal(object)       # {"ok": bool, "text": str}
    note = pyqtSignal(str)

    def __init__(self, endpoint, hz=20.0, window=DEFAULT_OBS_WINDOW, affinity=None,
                 parent=None):
        super().__init__(parent)
        self.endpoint = emaster_endpoint.Endpoint.parse(endpoint)
        self.hz = max(1.0, float(hz))
        self.window = max(1, int(window))
        self.affinity = affinity
        self._stop = False

        self.next_index = None      # 下一帧要取哪个序号；None = 还没对齐过
        self.gaps = 0               # 跳号发生的次数（每处记一次）
        self.missed = 0             # 估算漏掉的帧数
        self.resyncs = 0            # 因为落后太多、重新对齐的次数
        self.received = 0
        self.last_frame = None

    def stop(self):
        self._stop = True

    # ------------------------------------------------------------------
    def run(self):
        if self.affinity is not None:
            rt_affinity.pin_current_thread(self.affinity)
        self.link.emit({"ok": False, "text": f"观测：连接 {self.endpoint.display} 中…"})
        client = emaster_client.ObservationClient(self.endpoint, timeout=2.0)
        period = 1.0 / self.hz

        while not self._stop:
            if not self._connected(client):
                # 睡够重试间隔再试，别空转打主站的 accept。
                self._sleep(OBS_RECONNECT_S)
                continue

            started = time.monotonic()
            try:
                self._cycle(client)
            except emaster_client.CHANNEL_ERRORS as exc:
                self.link.emit({"ok": False,
                                "text": f"观测：断开（{type(exc).__name__}: {exc}）"})
                client.disconnect()
                self.next_index = None
                self._sleep(OBS_RECONNECT_S)
                continue

            # 按固定周期跑，而不是"干完一轮就立刻下一轮"——后者在主站特别快的时候
            # 会越转越快，白吃 CPU（那正是不能和主站抢的东西）。
            self._sleep(period - (time.monotonic() - started))

        client.disconnect()
        self.link.emit({"ok": False, "text": "观测：已停止"})

    def _connected(self, client):
        if client.connected:
            return True
        try:
            client.connect()
            client.read_info()
        except (emaster_client.CHANNEL_ERRORS + (emaster_endpoint.EndpointError,)) as exc:
            client.disconnect()
            self.link.emit({"ok": False,
                            "text": f"观测：连不上 {self.endpoint.display}"
                                    f"（{type(exc).__name__}: {exc}）"})
            return False
        self.next_index = None
        self.link.emit({"ok": True,
                        "text": f"观测：已连上 {client.display}"
                                f"（{client.axis_count} 轴，环 {client.ring_capacity} 槽）"})
        return True

    def _cycle(self, client):
        oldest, newest, capacity, empty = client.read_head()
        if empty:
            return
        if self.next_index is None:
            # 第一次对齐：从"最近 window 帧"开始，别一上来就倒 256 帧历史。
            self.next_index = max(oldest, newest - self.window)
        elif self.next_index < oldest:
            # 取数跟不上产出，还没读到的那几帧已经被覆盖了——这就是永久丢失。
            self.missed += oldest - self.next_index
            self.gaps += 1
            self.resyncs += 1
            self.note.emit(f"观测：落后于环，跳过了 {oldest - self.next_index} 帧"
                           f"（环容量 {capacity} 槽，取数 {self.hz:g} Hz × {self.window} 帧）")
            self.next_index = oldest
        if self.next_index >= newest:
            return      # 还没有新帧

        first, frames = client.read_window(self.next_index, self.window)
        if not frames:
            return
        # 服务端在第一个空洞处就停；事务头的起点是权威的，别拿 next_index 当准。
        if first > self.next_index:
            self.missed += first - self.next_index
            self.gaps += 1
        indices = [frame["publish_index"] for frame in frames]
        for before, after in zip(indices, indices[1:]):
            if after != before + 1:
                self.gaps += 1
                self.missed += after - before - 1

        self.next_index = indices[-1] + 1
        self.received += len(frames)
        self.last_frame = frames[-1]
        self.frames.emit({"frames": frames, "oldest": oldest, "newest": newest,
                          "capacity": capacity, "gaps": self.gaps,
                          "missed": self.missed, "resyncs": self.resyncs,
                          "received": self.received})

    def _sleep(self, seconds):
        """分段睡，别让 stop() 等一整个周期。"""
        deadline = time.monotonic() + max(0.0, seconds)
        while not self._stop and time.monotonic() < deadline:
            time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))


class CommandSession(QThread):
    """命令通道。**默认不存在**——界面一进来只连观测口。

    这一条是本轮的关键安全设计。命令口在服务端只有**一格** accept（backlog=1，且只在
    没有客户端时才 accept）：界面要是开机就占着它，台架脚本、别的工具就再也连不上了，
    而且症状是"挂住"不是报错，很难查。所以占它必须是一次显式动作。

    副作用也归零：只连观测口的时候，这个窗口**物理上不可能**让轴动起来。
    """

    snapshot = pyqtSignal(object)
    note = pyqtSignal(str)
    link = pyqtSignal(object)

    def __init__(self, endpoint, deployment, repo_root, jog_speed=DEFAULT_JOG_SPEED,
                 range_deg=DEFAULT_RANGE_DEG, tick_hz=CONTROL_RATE_HZ, status_hz=10.0,
                 affinity=None, parent=None):
        super().__init__(parent)
        self.endpoint = emaster_endpoint.Endpoint.parse(endpoint)
        self.deployment = deployment
        self.repo_root = repo_root
        self.jog_speed = jog_speed
        self.range_deg = range_deg
        self.tick_hz = tick_hz
        self.status_hz = max(0.5, min(status_hz, tick_hz))   # 不能比控制拍还密
        self.affinity = affinity
        self._stop = False
        self._requests = queue.Queue()
        self.engine = None
        # 每轴见过的最大偏移（度，相对启动位置，取绝对值）。**这是"轴到底动没动"的
        # 唯一凭据**：会话收尾时各轴都在起点，光看最后的快照，动过一轮和从头到尾
        # 没动是一模一样的。台架与离线回归都要靠它。
        self.peak_deg = []
        self.max_step_sent = 0      # 相邻两条目标之间真正发出的最大增量（counts）
        self._last_targets = None

    # ---- 给界面线程调用 -------------------------------------------------
    def request(self, action, *args):
        """投一个动作给控制线程。动作会排队，投完立刻返回——界面不阻塞。"""
        self._requests.put((action, args))

    def stop(self):
        self._stop = True

    # ------------------------------------------------------------------
    def run(self):
        if self.affinity is not None:
            rt_affinity.pin_current_thread(self.affinity)

        client = ConsoleClient(self.endpoint, timeout_s=1.0)
        engine = Engine(client, self.deployment, jog_speed=self.jog_speed,
                        range_deg=self.range_deg, repo_root=self.repo_root)
        self.engine = engine
        self.link.emit({"ok": False, "text": f"命令：连接 {self.endpoint.display} 中…"})

        period = 1.0 / self.tick_hz
        status_every = max(1, int(round(self.tick_hz / self.status_hz)))
        tick = 0
        last_message = None
        last_link = None

        while not self._stop:
            started = time.monotonic()
            self._drain(engine)
            engine.tick(started)
            tick += 1

            if tick % status_every == 0:
                # poll_status 同时充当心跳：服务端 5 秒没收到东西就单方面断开
                # （COMMAND_CLIENT_IDLE_TIMEOUT_S），10 Hz 的 status 远在它之前。
                engine.poll_status()
                self._track(engine)
                self.snapshot.emit(self._snapshot(engine))
                if engine.message != last_message:
                    last_message = engine.message
                    self.note.emit(engine.message)
                text = self._link_text(engine)
                if text != last_link:
                    last_link = text
                    self.link.emit({"ok": engine.attached, "text": text})

            self._sleep_until(started, period)

        engine.detach()
        client.disconnect()
        self.note.emit("命令：已释放（主站的外置目标看门狗在 200 ms 内收回控制权）")
        self.link.emit({"ok": False, "text": "命令：未接管"})

    def _drain(self, engine):
        while True:
            try:
                action, args = self._requests.get_nowait()
            except queue.Empty:
                return
            if not engine.attached:
                self.note.emit("命令：还没接上主站，这个动作先不做")
                continue
            handler = getattr(self, "_do_" + action, None)
            if handler is None:
                self.note.emit(f"命令：认不出的动作 {action}")
                continue
            handler(engine, *args)

    # ---- 动作。全部转给 Engine，界面这边不重写控制律 --------------------
    def _do_move(self, engine, pairs):
        engine.goto_relative(pairs)

    def _do_nudge(self, engine, index, degrees):
        direction = 1 if degrees >= 0 else -1
        engine.nudge(index, direction, abs(degrees))

    def _do_home(self, engine):
        engine.home_all()

    def _do_halt(self, engine):
        engine.halt_all()

    def _do_resume(self, engine):
        engine.resume_all()

    def _do_quick_stop(self, engine):
        engine.quick_stop_all()

    def _do_reanchor(self, engine):
        engine.reanchor()

    def _do_set_speed(self, engine, deg_per_s):
        engine.jog_speed = max(0.1, float(deg_per_s))

    # ---- 记账 -----------------------------------------------------------
    def _track(self, engine):
        """记下"轴走到过哪儿"和"真正发出的最大单步"。

        最大单步是从**发出去的那串目标**上算的，不是从本地账上算的：主站的单步限幅
        比的是相邻两条已提交目标，本地账算得再准也不算数。engine.last_sent 是主站
        真正收到的那一条。
        """
        if not self.peak_deg or len(self.peak_deg) != engine.axis_count:
            self.peak_deg = [0.0] * engine.axis_count
        for index in range(engine.axis_count):
            offset = engine.relative_degrees(index, engine._axis_position(index))
            if abs(offset) > abs(self.peak_deg[index]):
                self.peak_deg[index] = offset

        if engine.last_sent is not None and self._last_targets is not None:
            for before, after in zip(self._last_targets, engine.last_sent):
                self.max_step_sent = max(self.max_step_sent, abs(after - before))
        if engine.last_sent is not None:
            self._last_targets = list(engine.last_sent)

    # ---- 给界面看的快照 -------------------------------------------------
    def _link_text(self, engine):
        if not engine.attached:
            return f"命令：{engine.message}"
        return (f"命令：已接管 {engine.deployment_label()}"
                f"（已发 {engine.sent_count} 条目标）")

    def _snapshot(self, engine):
        """把界面要显示的东西**在控制线程里**抄成一份普通 dict。

        抄一份而不是让界面直接读 Engine：Engine 的几个列表是原地改的
        （控制拍就往 desired/commanded 里写），跨线程边改边读会读到撕裂的中间态。
        在这里抄，读到的每一格都来自同一个时刻。

        角度口径与 TUI 面板一致：**相对启动位置**，不是编码器绝对值。
        """
        axes = []
        for index in range(engine.axis_count):
            label, _color = engine.axis_state_label(index)
            axes.append({
                "index": index,
                "pos_deg": engine.relative_degrees(index, engine._axis_position(index)),
                "planned_deg": engine.relative_degrees(index, engine._axis_planned(index)),
                "desired_deg": engine.relative_degrees(index, engine.desired[index]),
                "peak_deg": self.peak_deg[index] if index < len(self.peak_deg) else 0.0,
                "status_word": engine.axis_status_word(index),
                "error_code": engine.axis_error_code(index),
                "state_label": label,
                "headroom": engine.range_headroom(index),
            })
        return {
            "attached": engine.attached,
            "state": engine.master_state,
            "halted": engine.halted,
            "sent_count": engine.sent_count,
            "max_step_sent": self.max_step_sent,
            "axis_count": engine.axis_count,
            "deployment": engine.deployment,
            "jog_speed": engine.jog_speed,
            "range_deg": engine.range_deg,
            "ticks_left": engine.ticks_left,
            "axes": axes,
        }

    def _sleep_until(self, started, period):
        deadline = started + period
        while not self._stop and time.monotonic() < deadline:
            time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))


def repo_root_of(module_file):
    """从 tools/emaster_gui/xxx.py 反推仓库根。Engine 要用它去读部署配置。"""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(module_file))))


def command_preflight(deployment, repo_root):
    """接管之前先过一遍模式护栏：返回 (能不能接管, 拒绝的理由)。

    **为什么要在起线程之前问一次。** 护栏本身在 Engine.attach() 里，也会拦住——但它
    拦得晚，会话线程已经起来了，界面于是收到一份 attached=False、state=-1、一根轴都
    没有的快照，显示成"主站状态未知"的空白轴表。真正的理由（"这是 CST 部署，面板发
    的是位置目标"）只落在消息区里，很容易看漏。而看漏的后果是：操作员以为界面坏了，
    去反复点接管。

    这只是**提前问一声**，护栏仍在 attach() 里守着，两处问的是同一个方法，不是两份
    判据。Engine 的构造函数不碰网络（只存字段），所以拿一个没人用的 client 来问是安全的。
    """
    engine = Engine(None, deployment, repo_root=repo_root)
    if engine.mode_guard_passed():
        return True, None
    return False, engine.message
