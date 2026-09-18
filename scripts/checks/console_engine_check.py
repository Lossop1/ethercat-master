#!/usr/bin/env python3
"""控制台的离线回归：不连主站、不要终端，把引擎层和按键层跑一遍。

本机 Python 没有 AF_UNIX，跑不起假主站的进程版，所以这里把"假主站"做成一个
内存里的客户端替身：command() 直接返回按同一套协议拼出来的响应串，
refresh_status/refresh_topology 用的仍是控制台里那套真解析代码。
假主站复刻了真主站两条要命的行为：单步超限打掉整个会话、状态字 bit11 = 限位。

改一次 `tools/emaster_console.py` 就跑一次，用法（在仓库根目录）：

    PYTHONIOENCODING=utf-8 python scripts/checks/console_engine_check.py --selftest
    PYTHONIOENCODING=utf-8 python scripts/checks/console_engine_check.py --mutate-startup-window

后者是启动窗口那条检查的对抗性对照：把 `factor_of` 的边界检查拿掉、退回
`self.factor[index]`，那条检查**必须变红**。它验证的是检查本身有没有判别力——
2026-09-18 那次面板一帧就死（`console.sh --start --obs`，主站没人看着跑到两根轴进故障）
正是这个下标，检查要是抓不住它，跑绿了也不说明任何事。

台架那一半（真主站 + 真电机）在 `scripts/bench_panel_drive.sh` 与
`scripts/bench_panel_jog.sh`，转录用 `scripts/analysis/panel_transcript.py` 解析。
"""
import importlib.util
import os
import pathlib
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

# 对照点：带边界检查的写法 → 去掉边界、退回裸下标。
MUTATE_STARTUP_FROM = """        if index < 0 or index >= len(self.factor):
            return None
        return self.factor[index]
"""
MUTATE_STARTUP_TO = """        return self.factor[index]
"""


def install_mutated_console():
    """把去掉边界的 `emaster_console.py` 装进 sys.modules，供启动窗口对照用。

    只在临时目录里生成副本，仓库文件一个字节都不动。
    """
    source_path = ROOT / "tools" / "emaster_console.py"
    source = source_path.read_text(encoding="utf-8")
    if MUTATE_STARTUP_FROM not in source:
        print("变异点找不到——factor_of 里那句边界检查的写法变了，这个对照得跟着改。",
              file=sys.stderr)
        return False
    handle = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False,
                                         encoding="utf-8")
    with handle:
        handle.write(source.replace(MUTATE_STARTUP_FROM, MUTATE_STARTUP_TO, 1))
    spec = importlib.util.spec_from_file_location("emaster_console", handle.name)
    module = importlib.util.module_from_spec(spec)
    sys.modules["emaster_console"] = module
    spec.loader.exec_module(module)
    return True


MUTATING = "--mutate-startup-window" in sys.argv
if MUTATING and not install_mutated_console():
    sys.exit(2)

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
    if MUTATING:
        # 对照模式：只跑启动窗口那一条，并且期望它**红**。
        code = startup_window_check()
        if code == 0:
            print("\n对照失败：去掉边界后这条检查仍是绿的——它抓不住那次崩溃，等于摆设")
            return 1
        print("\n对照通过：旧写法（裸下标）下这条检查确实变红")
        return 0

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
    if startup_window_check() != 0:
        return 1
    return mode_guard_check()


def startup_window_check():
    """主站"还没进 RUNNING"那段窗口里，面板一帧都不许崩。

    现场（2026-09-18）：`console.sh --start --obs` 把主站拉起来，面板第一帧就
    IndexError 退出——而主站已经起来了、没人看着它，一直跑到两根轴进故障。

    机理是这个窗口里**两个列表不是一起到的**：`status` 已经能报回五根轴（填
    `engine.axes`），而 `factor`（每轴 counts/度，要读 `topology` 才填）还是空的
    ——attach() 里"主站还在启动"那一步排在读拓扑**之前**就 return 了。而画面上那条
    "软范围余量"是无条件拿 factor 换算的，`self.factor[index]` 当场下标越界。它发生
    在**每次重画**的路径上，所以不是偶发，是必崩。

    这个窗口还有个后果值得记住：面板崩了以后主站不会跟着退出（它是独立进程组），
    于是"主站开着、没人监控"这条纪律会被一个崩溃悄悄违反。
    """
    failures = []
    client = FakeClient("/tmp/fake-emaster.sock")
    client.master.state = 2               # 主站起来了，但还没进 RUNNING
    engine = Engine(client, "fake-bench", 10.0, 45.0,
                    repo_root=os.path.dirname(os.path.dirname(
                        os.path.abspath(__file__))))
    if engine.attach():                   # 应当失败：主站还在启动
        print("前置条件不成立：这个窗口里 attach() 不该成功")
        return 1
    if not engine.axes or engine.factor:
        print(f"前置条件不成立：axes={len(engine.axes)} 根、factor={len(engine.factor)} 个"
              "——要复刻的是「有轴、没系数」")
        return 1

    # 崩溃点就在这些换算上；逐个调一遍，一个都不许抛。
    calls = [("软范围余量", lambda: engine.range_headroom(0)),
             ("软范围", lambda: engine.range_limits(0)),
             ("角度换算", lambda: engine.counts_to_deg(0, 1234)),
             ("角度→counts", lambda: engine.deg_to_counts(0, 30.0)),
             ("单步上限", lambda: engine.step_limit_counts(0))]
    for label, call in calls:
        try:
            call()
        except Exception as exc:          # noqa: BLE001 —— 这里要的就是"什么都别抛"
            failures.append(f"{label} 在启动窗口里抛了 {type(exc).__name__}: {exc}")

    # 系数没到手时"余量"必须是"不知道"，不能编一个数出来（编出来会被当真的用）。
    # 这里也要接住异常：上面那一轮已经记过一笔了，再让异常冲出检查就等于用一个
    # 回溯代替结论——判别力还在，但读的人只看到退栈，看不出是哪一条不过。
    try:
        if engine.range_headroom(0) is not None:
            failures.append("系数没到手时余量应当是不显示（None），不是编个数")
    except Exception as exc:              # noqa: BLE001
        failures.append(f"软范围余量取值时抛了 {type(exc).__name__}: {exc}")

    # 越界轴号也得挡住：轴号来自按键，key 层和这里都可能先于拓扑拿到轴号。
    for label, call in [("越界轴号的软范围", lambda: engine.range_limits(AXES + 3)),
                        ("越界轴号的余量", lambda: engine.range_headroom(AXES + 3))]:
        try:
            call()
        except Exception as exc:          # noqa: BLE001
            failures.append(f"{label}抛了 {type(exc).__name__}: {exc}")

    if failures:
        print("\n启动窗口失败项：" + "，".join(failures))
        return 1
    print("启动窗口通过：主站还在启动时面板不崩，余量显示为未知")
    return 0


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

    # 快停是 Engine 上给图形界面用的第二个急停入口（TUI 的 q 走 halt）。两条都
    # 得冻结目标：不冻的话，快停刚结束、下一个控制拍就把刚才那批目标重发一遍。
    engine.halted = False
    engine.desired[1] = engine.commanded[1] + 3000
    engine.quick_stop_all()
    check("快停也冻结目标",
          engine.desired[1] == engine.commanded[1] and engine.halted
          and engine.ticks_left == 0,
          f"desired={engine.desired[1]} commanded={engine.commanded[1]} "
          f"ticks_left={engine.ticks_left}")
    engine.halted = False

    if failures:
        print("\n按键层失败项：" + "，".join(failures))
        return 1
    print("按键层全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
