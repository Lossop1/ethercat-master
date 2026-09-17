#!/usr/bin/env python3
"""控制台的离线回归：不连主站、不要终端，把引擎层和按键层跑一遍。

本机 Python 没有 AF_UNIX，跑不起假主站的进程版，所以这里把"假主站"做成一个
内存里的客户端替身：command() 直接返回按同一套协议拼出来的响应串，
refresh_status/refresh_topology 用的仍是控制台里那套真解析代码。
假主站复刻了真主站两条要命的行为：单步超限打掉整个会话、状态字 bit11 = 限位。

改一次 `tools/emaster_console.py` 就跑一次，用法（在仓库根目录）：

    PYTHONIOENCODING=utf-8 python scripts/checks/console_engine_check.py --selftest

台架那一半（真主站 + 真电机）在 `scripts/bench_panel_drive.sh` 与
`scripts/bench_panel_jog.sh`，转录用 `scripts/analysis/panel_transcript.py` 解析。
"""
import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))

from emaster_console import ConsoleClient, Engine, selftest  # noqa: E402

AXES = 5
MAX_STEP = 6400
COUNTS_PER_DEG = 16384.0 * 28.0 / 360.0


class FakeMaster:
    """真主站行为的简化模型。"""

    def __init__(self):
        self.state = 4
        self.cycle = 0
        self.planned = [0] * AXES
        self.actual = [0.0] * AXES
        self.halted = False
        self.last_update = 0.0
        self.aborts = []
        self.steps = []          # 每次 set_external_target 的相邻增量，供核对
        self._last = time.monotonic()

    def _advance(self):
        now = time.monotonic()
        if now - self._last < 0.005:
            return
        self.cycle += 1
        self._last = now
        for i in range(AXES):
            if self.halted:
                continue
            delta = self.planned[i] - self.actual[i]
            self.actual[i] += max(-3000.0, min(3000.0, delta))

    def handle(self, line):
        self._advance()
        parts = line.split()
        if not parts:
            return "ERROR|空命令"
        name = parts[0]
        if name == "status":
            body = f"OK|state={self.state} cycle={self.cycle} axes={AXES} enabled=1 completed=0"
            for i in range(AXES):
                # halt 只是控制字 bit8，不改变 CiA402 状态机，所以状态字仍是使能。
                word = 0x0027
                if abs(self.actual[i] - self.planned[i]) < 2:
                    word |= 0x0400
                body += (f"|a{i + 1}:pos={int(self.actual[i])},vel=0,torque=0,"
                         f"status=0x{word:04x},target_pos={self.planned[i]},"
                         f"planned={self.planned[i]},err=0x0000,state=6")
            return body
        if name == "topology":
            body = f"OK|axes={AXES},max_step={MAX_STEP}"
            for i in range(AXES):
                body += f"|a{i + 1}:bus={i + 1},enc=16384,gear=28/1,torque=0"
            return body
        if name == "halt":
            self.halted = len(parts) > 1 and parts[1] == "1"
            return f"OK|halt {'set' if self.halted else 'cleared'}"
        if name == "fault_reset":
            return "OK|fault reset requested"
        if name == "set_external_target":
            if self.state != 4:
                return "ERROR|NOT_RUNNING"
            if len(parts) - 1 != AXES:
                return f"ERROR|需要 {AXES} 个目标"
            values = [int(v) for v in parts[1:]]
            deltas = [abs(v - p) for v, p in zip(values, self.planned)]
            self.steps.append(max(deltas))
            if max(deltas) > MAX_STEP:
                self.state = 0     # 真主站：中止整个会话
                self.aborts.append(max(deltas))
                return f"ERROR|MOTION_INVALID（单步 {max(deltas)} > {MAX_STEP}）"
            self.planned = values
            self.last_update = time.monotonic()
            return "OK|target accepted"
        return "ERROR|unknown command"


class FakeClient(ConsoleClient):
    def __init__(self, sock_path):
        super().__init__(sock_path)
        self.master = FakeMaster()
        self.sock = None

    def connect(self):
        self.sock = "fake"
        return True

    def disconnect(self):
        self.sock = None

    def command(self, cmd, retries=2):
        return self.master.handle(cmd)


class Args:
    def __init__(self, travel):
        self.socket = "/tmp/fake-emaster.sock"
        self.deployment = "fake-bench"
        self.repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.jog_speed = 10.0
        self.range = 45.0
        self.start = False
        self.travel = travel
        self.allow_stop_master = False


def main():
    args = Args(travel=1.0)
    created = []

    def factory(path):
        client = FakeClient(path)
        created.append(client)
        return client

    # selftest 会先看套接字在不在，假主站没有套接字文件，所以这里把检查绕开：
    # 造一个空的 socket 文件（Windows 上普通空文件就行，os.path.exists 只认存在）。
    open(args.socket, "w").close()
    try:
        code = selftest(args, client_factory=factory)
    finally:
        os.unlink(args.socket)

    master = created[-1].master
    worst = max(master.steps) if master.steps else 0
    print(f"\n假主站记录：发出 {len(master.steps)} 条目标，"
          f"最大相邻增量 {worst} counts（上限 {MAX_STEP}），"
          f"会话中止 {len(master.aborts)} 次")

    # 负对照：直接给假主站一步大跳，确认它的中止判据是活的、不是摆设。
    # 它要是不会中止，上面"最大相邻增量 ≤ 上限"这条通过就说明不了任何事。
    jumped = master.handle("set_external_target " + " ".join(["999999"] * AXES))
    print(f"负对照：故意发一步 999999 → {jumped[:60]}…  state={master.state}")
    if master.state != 0 or not master.aborts:
        print("负对照失败：假主站没有中止，说明抑制判据没接上")
        return 1
    print("负对照通过：假主站确实会因单步超限打掉会话")

    if code != 0:
        return code

    # 负对照刚刚把假主站打到 state=0，按键层要重新起一个能跑的引擎。
    master.state = 4
    engine = Engine(created[-1], args.deployment, args.jog_speed, args.range,
                    repo_root=args.repo)
    if not engine.wait_attached(5.0):
        print("按键层起不来：" + engine.message)
        return 1
    if key_layer_check(engine, master) != 0:
        return 1
    return mode_guard_check()


def mode_guard_check():
    """模式护栏：非位置模式的部署必须被面板拒绝，位置模式的照常放行。

    用的是仓库里真有的两份部署（quint = csp、cst-smoke = cst），不是造的假配置——
    护栏要挡的正是"这份配置真的存在、也真的能起主站"的那种情况。
    """
    root = str(pathlib.Path(__file__).resolve().parents[2])
    failures = []

    def check(label, condition, detail=""):
        print(f"  [{'通过' if condition else '失败'}] {label} {detail}".rstrip())
        if not condition:
            failures.append(label)

    print("\n6) 模式护栏")
    csp = Engine(FakeClient("/tmp/fake-csp.sock"), "orangepi-bench-quint-30deg",
                 repo_root=root)
    check("位置模式（csp）放行", csp.mode_guard_passed(),
          f"mode={csp._selected_mode or '(读不到)'}")

    cst = Engine(FakeClient("/tmp/fake-cst.sock"), "orangepi-bench-cst-smoke",
                 repo_root=root)
    check("力矩模式（cst）拒绝", not cst.mode_guard_passed(), cst.message)
    # attach 的第一件事就是这个判断——被拒时不该碰套接字（假客户端没有连接）。
    check("被拒时 attach 直接返回，不连套接字", not cst.attach())

    unknown = Engine(FakeClient("/tmp/fake-none.sock"), "no-such-deployment",
                     repo_root=root)
    check("读不到部署时不拦（护栏不猜）", unknown.mode_guard_passed())

    if failures:
        print("\n模式护栏失败项：" + "，".join(failures))
        return 1
    print("模式护栏全部通过")
    return 0


def key_layer_check(engine, master):
    """按键层：这一层在台架上得有人坐在键盘前才验得了，这里先跑一遍。"""
    from emaster_console import Panel

    panel = Panel(engine, master, Args(travel=1.0))
    failures = []

    def check(label, condition, detail=""):
        print(f"  [{'通过' if condition else '失败'}] {label} {detail}".rstrip())
        if not condition:
            failures.append(label)

    print("\n5) 按键层")
    engine.selected_axis = 0
    base = engine.desired[0]

    panel.handle_key("3")
    check("数字键选轴", engine.selected_axis == 2, f"选中 轴{engine.selected_axis + 1}")
    check("选轴不改目标", engine.desired[0] == base)

    # 点动：按下就走，按住连续，松手停。时间用假钟推，免得真等。
    fake = [1000.0]
    panel.clock = lambda: fake[0]
    period = 1.0 / 50.0
    before = engine.desired[2]
    panel.handle_key("+")
    fake[0] += period
    panel.step_jog(fake[0])
    check("按下就动（不等终端自动重复）", engine.desired[2] > before,
          f"第一拍走了 {engine.desired[2] - before} counts")

    held_start = engine.desired[2]
    for _ in range(15):                       # 按住 0.6 秒（25 次/秒的按键重复 + 50Hz 控制拍）
        panel.handle_key("+")
        for _ in range(2):
            fake[0] += period
            panel.step_jog(fake[0])
    held = engine.counts_to_deg(2, engine.desired[2] - held_start)
    check("按住是连续走（0.6 秒走了 3° 以上）", held > 3.0, f"{held:.2f}°")

    panel.handle_key(" ")                     # 按别的键 = 松手
    fake[0] += period
    panel.step_jog(fake[0])
    for _ in range(10):
        fake[0] += period
        panel.step_jog(fake[0])
    settled = engine.desired[2]
    for _ in range(10):
        fake[0] += period
        panel.step_jog(fake[0])
    check("松手后停住", engine.desired[2] == settled,
          f"松手后又走了 {engine.desired[2] - settled} counts")

    panel.handle_key("[")
    check("没跑往返时方括号不动目标", engine.desired[2] == settled)
    check("方括号给出提示", "往返振幅" in (panel.notice or ""), panel.notice)

    # 一路顶到软范围：不能再越界
    for _ in range(400):
        fake[0] += period
        panel.handle_key("+")
        panel.step_jog(fake[0])
        if engine.desired[2] == int(engine.range_limits(2)[1]):
            break
    edge = engine.range_limits(2)[1]
    check("点动被软范围夹住", engine.desired[2] == int(edge),
          f"停在 {engine.counts_to_deg(2, engine.desired[2]):+.2f}°，边界 "
          f"{engine.counts_to_deg(2, edge):+.2f}°")
    check("夹住时给出提示", "软范围" in (panel.engine.message or ""), panel.engine.message)
    panel.handle_key(" ")                     # 松手，别让它带着点动状态往下走

    # ---- 输入行：整行在面板内编辑，控制流不断 ----
    panel.handle_key("g")
    check("g 打开输入行（面板不退出、不阻塞）", panel.input_buffer == "")
    for ch in "1:0.5":
        panel.handle_key(ch)
    check("输入行收键", panel.input_buffer == "1:0.5", panel.input_buffer)
    panel.handle_key("\x7f")
    check("退格删一个字符", panel.input_buffer == "1:0.", panel.input_buffer)
    panel.handle_key("5")
    panel.handle_key("\r")
    check("回车提交后输入行关掉", panel.input_buffer is None)
    check("轴1 走到 起点+0.5°",
          abs(engine.relative_degrees(0, engine.desired[0]) - 0.5) < 0.01,
          f"{engine.relative_degrees(0, engine.desired[0]):+.3f}°")
    check("上一条目标记下来了（供姿态槽用）", panel.last_targets == [(0, 0.5)],
          str(panel.last_targets))

    other = engine.desired[1]
    panel.handle_key("g")
    panel.handle_key("30")
    panel.handle_key("ESC")
    check("Esc 取消这一行", panel.input_buffer is None and engine.desired[1] == other)
    panel.handle_key("g")
    panel.handle_key("坏")
    panel.handle_key("\r")
    check("看不懂的输入只给提示、不掉目标", engine.desired[1] == other
          and "没看懂" in (panel.notice or ""), panel.notice)

    # ---- 多轴一行走：1:30 4:12 ----
    engine.home_all()
    panel.handle_key("g")
    for ch in "2:0.4 4:0.8":
        panel.handle_key(ch)
    panel.handle_key("\r")
    check("多轴同时起停：两根轴一起排进同一个拍数",
          engine.ticks_left > 0
          and abs(engine.relative_degrees(1, engine.desired[1]) - 0.4) < 0.01
          and abs(engine.relative_degrees(3, engine.desired[3]) - 0.8) < 0.01,
          f"ticks_left={engine.ticks_left}")
    check("没点到的轴不动", engine.desired[0] == engine.commanded[0])

    # ---- 姿态槽 ----
    panel.handle_key("s")
    check("s 之后等第二个键", panel.pending_prefix == "s")
    panel.handle_key("2")
    check("s 2 存住了刚才那条目标", panel.poses[2] == [(1, 0.4), (3, 0.8)],
          str(panel.poses[2]))
    engine.home_all()
    panel.handle_key("v")
    panel.handle_key("2")
    check("v 2 取出来再走一遍",
          abs(engine.relative_degrees(3, engine.desired[3]) - 0.8) < 0.01,
          panel.pose_text(2))
    panel.handle_key("v")
    panel.handle_key("3")
    check("空槽只给提示", "空的" in (panel.notice or ""), panel.notice)

    # ---- 调速 / 回起点 / 重锚零点 ----
    speed = engine.jog_speed
    panel.handle_key(".")
    check("句号提速", engine.jog_speed > speed, f"{speed:g} → {engine.jog_speed:g}°/s")
    panel.handle_key(",")
    check("逗号降速", abs(engine.jog_speed - speed) < 1e-9, f"{engine.jog_speed:g}°/s")
    engine.desired[0] += 1000
    panel.handle_key("0")
    check("0 回启动位置", engine.desired[0] == engine.origin[0],
          f"{engine.relative_degrees(0, engine.desired[0]):+.3f}°")
    engine.commanded[0] += 1000
    panel.handle_key("z")
    check("z 重新锚零点", engine.origin[0] == engine._axis_position(0))

    # ---- 往返验证的开与关 ----
    panel.handle_key("t")
    check("t 起往返验证", panel.soak is not None and panel.soak.active,
          panel.notice or "")
    amplitude = panel.soak.amplitude
    panel.handle_key("]")
    check("往返跑着时 [ / ] 调振幅", panel.soak.amplitude > amplitude,
          f"{amplitude:g} → {panel.soak.amplitude:g}°")
    panel.handle_key("t")
    check("再按 t 停下", not panel.soak.active, panel.notice or "")
    panel.handle_key("h")
    check("h 打开帮助", panel.show_help)
    panel.handle_key("x")
    check("任意键关掉帮助", not panel.show_help)

    engine.commanded[2] = engine.desired[2] - 1000
    panel.handle_key("q")
    check("急停冻结目标", engine.desired[2] == engine.commanded[2] and engine.halted)

    if failures:
        print("\n按键层失败项：" + "，".join(failures))
        return 1
    print("按键层全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
