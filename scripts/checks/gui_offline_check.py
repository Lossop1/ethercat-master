#!/usr/bin/env python3
"""无头跑图形界面，对着**真 socket** 的假主站走一遍，钉住这一层的安全性质。

跑法：python scripts/checks/gui_offline_check.py
      界面在 QT_QPA_PLATFORM=offscreen 下自己起在子进程里，不需要显示器。

为什么这个检查值得存在，而不是"手工点一遍就行"：

- 这一层最要紧的性质是**默认只读**。它不是一个能靠看界面看出来的东西（窗口长得
  一模一样），而它一旦坏了，后果是"主站开着的时候台架脚本再也连不上命令口"——
  而且表现为挂住，不报错。所以要用假主站的**命令计数**去证明：没点接管之前，
  命令口一个字节都不该收到。
- 第二要紧的是**限幅**。单步超限在真主站上不是报错，是打掉整个会话，而那条命令
  早就回了 OK。这一层要是把步长算大了，检查里看得见的只有"会话没了"。
  所以直接对着**真发出去的目标序列**量相邻增量。

假主站是 scripts/checks/fake_master.py（它自己另有一个忠实性检查）。界面在这条
链路上是黑盒：只看假主站的计数和界面自己在自检里打出来的那些事实。

没证到的写在末尾"没证到的"一节。
"""

import argparse
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
import rt_affinity  # noqa: E402

TOOLS = os.path.join(ROOT, "tools")

# 假主站的单步限幅，与配置里那个 6400 一致。界面的步长必须落在它的 0.8 倍以内。
MAX_STEP_MARGIN = 0.8


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


def gui_env():
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def run_gui(master, extra, seconds):
    """跑一轮界面，返回 (退出码, 自检报告文本)。

    给界面的是**观测端点单独指定**，命令端点用 --tcp 推出来——两处都不是
    「碰巧一样」，这样"它到底连了哪条通道"才有意义。
    """
    args = [sys.executable, "-m", "emaster_gui",
            "--obs-endpoint", f"tcp:127.0.0.1:{master.obs_port}",
            "--tcp", f"127.0.0.1:{master.port}",
            "--selftest", str(seconds), "--no-confirm"] + list(extra)
    done = subprocess.run(args, cwd=TOOLS, env=gui_env(), capture_output=True,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=seconds + 60)
    want(done.returncode == 0,
         f"界面自己退出了 {done.returncode}\nstdout={done.stdout}\n"
         f"stderr={done.stderr[-2000:]}")
    report = done.stdout
    want("---- GUI 自检 ----" in report, f"没拿到自检报告：{report!r}")
    return done.returncode, report


def field(report, prefix):
    """从自检报告里取一行。取不到就报错——报告的形状变了不能装作没变。"""
    for line in report.splitlines():
        if line.startswith(prefix):
            return line
    raise CheckFailed(f"自检报告里没有以 {prefix!r} 开头的行：\n{report}")


# ---------------------------------------------------------------- 只读默认

def check_readonly_by_default(context):
    """没点接管，命令口**一个字节都不该收到**——哪怕命令端点已经给了。"""
    with fake_master.FakeMaster(publish_hz=1000.0) as master:
        _, report = run_gui(master, [], 4.0)
        counters = master.snapshot_counters()
        want(counters["commands"] == 0,
             f"只读模式下命令口收到了 {counters['commands']} 条命令——"
             f"默认就不该连命令口")
        # 观测那头要真的在跑，否则"没连命令口"可能只是整个都连不上。
        want(counters["obs_accepts"] >= 1, f"观测口没被连上：{counters}")
        want("接管中：否" in report, f"报告里说是接管状态：{report}")
        want("轴表：没有快照" in report, f"没接管却拿到了快照：{report}")


def check_observation_keeps_up(context):
    """观测取数要跟得上产出，且**只用一条连接**（不反复重连打主站的 accept）。"""
    with fake_master.FakeMaster(publish_hz=1000.0) as master:
        _, report = run_gui(master, [], 6.0)
        counters = master.snapshot_counters()
        want(counters["obs_accepts"] == 1,
             f"观测口被连了 {counters['obs_accepts']} 次——应该只连一次，"
             f"反复重连会把自己踢下线")
        want(counters["clients_replaced_obs"] == 0,
             f"观测口发生了 {counters['clients_replaced_obs']} 次顶替")

        got = int(field(report, "观测：收到").split("收到")[1].split("帧")[0].strip())
        # 主站产 1000 帧/秒，跑了 6 秒。取数能力 20 Hz × 64 帧 = 1280 帧/秒，
        # 够用但只快一点点，所以留出启动那一下的余量，按 85% 判。
        want(got >= 1000 * 6 * 0.85,
             f"6 秒只收到 {got} 帧，跟不上主站的 1000 帧/秒")
        want("跳号 0 处" in report, f"掉帧了：{field(report, '观测：收到')}")
        want("估算丢失 0 帧" in report, f"有丢失：{field(report, '观测：收到')}")
        want("重新对齐 0 次" in report, f"发生了重新对齐：{field(report, '观测：收到')}")
        want("帧距=1.000ms" in report,
             f"帧距不是 1.000 ms：{field(report, '观测：publish_index')}")


# ---------------------------------------------------------------- 接管

def check_takeover_moves_within_limits(context):
    """接管 → 动起来 → 释放。要同时钉住：真的发了目标、单步没超限、轴真的动过。"""
    with fake_master.FakeMaster(publish_hz=1000.0) as master:
        _, report = run_gui(master, ["--selftest-takeover"], 7.0)
        counters = master.snapshot_counters()
        want(counters["commands"] > 0, "接管了却一条命令都没发")
        want(counters["step_violations"] == 0,
             f"发出去的目标超过了单步限幅：{counters['step_violations']} 次")
        want(counters["aborts"] == 0,
             f"会话被打掉了（单步超限的后果）：{counters['aborts']} 次")

        sent = int(field(report, "主站：state").split("已发")[1].split("条")[0].strip())
        want(sent > 0, f"自检报告说一条目标都没发：{report}")
        largest = int(field(report, "主站：state").split("最大增量")[1]
                      .split("counts")[0].strip())
        ceiling = int(fake_master.DEFAULT_MAX_STEP * MAX_STEP_MARGIN)
        want(largest <= ceiling,
             f"相邻目标最大增量 {largest} counts 超过了上限 {ceiling}"
             f"（max_step {fake_master.DEFAULT_MAX_STEP} × {MAX_STEP_MARGIN}）")

        # 轴到底动没动。收尾时各轴都在起点，只有"曾到过"能证明。
        moved = [line for line in report.splitlines() if "曾到过" in line]
        want(len(moved) == fake_master.DEFAULT_AXES,
             f"报告里只有 {len(moved)} 行的轴信息")
        axis1 = moved[0]
        want("曾到过=+0.200°" in axis1,
             f"轴1 没有按自检计划走那 0.2°：{axis1}")
        for line in moved[1:]:
            want("曾到过=+0.000°" in line,
                 f"没让它动的轴却动了：{line}")
        want("状态字=0x0427" in axis1, f"状态字不对（应当就绪）: {axis1}")


def check_charts_receive_and_decimate(context):
    """曲线要真的收到帧、真的抽了稀，而且**只喂当前显示的那根轴**。

    "只喂一根"这条在离线检查里看着像小事，实际是这一层唯一能自己决定的内存开销：
    五根全收就是五倍。它也是个**看不见的性质**——界面上五根轴的图长得一样（都没画），
    只有第 1 根背后有数据。所以要在这里钉住。

    形状对不对不归这条管（那是 chart_check.py 的事）。
    """
    with fake_master.FakeMaster(publish_hz=1000.0) as master:
        _, report = run_gui(master, ["--chart-seconds", "10"], 6.0)

        want("曲线：5 根轴" in report,
             f"曲线控件没按 5 根轴建起来：{field(report, '曲线：')}")
        want("当前显示轴1" in report, f"默认显示的不是第 1 根轴：{field(report, '曲线：')}")

        line = field(report, "  轴1:")
        want("喂了" in line and "存成" in line, f"轴1 那行的形状变了：{line}")
        fed = int(line.split("喂了")[1].split("帧")[0].strip())
        buckets = int(line.split("存成")[1].split("个桶")[0].strip())
        want(fed > 0, f"轴1 一帧都没喂到：{line}")
        # 10 秒窗、1 kHz → 10 ms 一桶，6 秒最多 600 个桶；喂进来的是几千帧。
        want(buckets < fed / 5,
             f"喂了 {fed} 帧存了 {buckets} 个桶，抽稀没起作用（窗口 10 秒）")
        want("断开 0 处" in line, f"取数没掉帧，曲线却断了：{line}")
        want("时间倒序 0 帧" in line, f"帧的时间倒着来过：{line}")

        # 没显示的那几根轴不该收数据。这是有意的设计，不是漏了。
        for axis in range(2, fake_master.DEFAULT_AXES + 1):
            other = field(report, f"  轴{axis}:")
            want("没喂过帧" in other,
                 f"轴{axis} 不是当前显示的却收了数据，五倍内存那条设计破了：{other}")


def check_window_fits_on_a_normal_screen(context):
    """窗口的最小宽度要放得进一块普通屏。

    **这条守的是一个能反复复发、而且症状是"看不见急停按钮"的坑。** 布局的最小宽度不是
    常量：中文说明文字没有空格、自动折行找不到断点，Qt 会把整句话的宽度当最小宽度报上来；
    跨列摆的话还会每跨一列记一遍。这条界面上连着踩过两次——动作区一行摆全部按钮是
    2127 px，改成网格还是 1586 px。两种情况下窗口都拉不到那么宽，**右边那一排按钮
    （包括急停）就永远在屏幕外**，而没有任何报错。

    撞坏了会怎么样这里写清楚：不是画错，是有一个按钮你永远点不到。所以哪怕屏幕再宽，
    这个数也该有人看着。
    """
    # 1280 是"最常见的笔记本窄边"，比它宽就有一批机器放不下。
    CEILING = 1280
    with fake_master.FakeMaster(publish_hz=200.0) as master:
        _, report = run_gui(master, [], 3.0)
        line = field(report, "界面：最小宽度")
        width = int(line.split("最小宽度")[1].split("px")[0].strip())
        want(width <= CEILING,
             f"窗口最小宽度 {width} px 超过 {CEILING}——布局又被某段文字或某一行控件"
             f"顶宽了，窄屏上右侧按钮会看不见：{line}")
        chart_line = field(report, "界面：曲线区")
        got = int(chart_line.split("拿到")[1].split("px")[0].strip())
        want("至少要 210" in chart_line and got >= 210,
             f"曲线区没拿到它要的高度，三个横条会被压成细缝：{chart_line}")


def check_command_slot_is_held(context):
    """接管期间命令口那一格**确实被占着**——别的客户端进不来。

    占着的时候第二个客户端会挂住（服务端 backlog=1 且只在空闲时 accept）。这正是
    台架脚本会遇到的症状，所以这里就按那个症状判：连得上（TCP 层会排进 backlog），
    但永远拿不到回复。
    """
    master = fake_master.FakeMaster(publish_hz=200.0)
    seconds = 6.0
    with master.start():
        args = [sys.executable, "-m", "emaster_gui",
                "--obs-endpoint", f"tcp:127.0.0.1:{master.obs_port}",
                "--tcp", f"127.0.0.1:{master.port}",
                "--tick-hz", "20", "--status-hz", "10",
                "--selftest", str(seconds), "--selftest-takeover", "--no-confirm"]
        child = subprocess.Popen(args, cwd=TOOLS, env=gui_env(),
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace")

        # 自检计划里接管排在 10% 处，等它进去再探。
        time.sleep(seconds * 0.10 + 1.5)
        want(master.snapshot_counters()["commands"] > 0,
             "还没接管上，这一轮探不出「占住」这件事")
        want(_probe_command(master, timeout=1.5) is False,
             "接管期间第二个客户端**连上并得到了回复**——命令口那一格没被占住")

        out, _ = child.communicate(timeout=seconds + 60)
        want(child.returncode == 0, f"界面退出码 {child.returncode}：{out[-2000:]}")


def check_release_actually_releases(context):
    """释放之后那一格要还回去——**并且在同一个活着的进程里探**。

    这条本来写在上面那个检查的结尾（"界面退出之后再探一次"），实测**测不出东西**：
    把释放改成不 stop、直接把会话丢掉，那一版照样 7/7 通过——因为进程一退，它占的
    套接字自然就关了，探谁都是通的。也就是说那一版证的是"进程退干净了"，不是"释放
    生效了"。所以挪到这里：同一个进程里起会话、停会话、再探，进程一直活着。

    只读观测那条通道不牵扯进来：这里不连观测口。
    """
    try:
        from PyQt5.QtCore import QCoreApplication          # noqa: F401
        from emaster_gui import poller
    except ImportError as exc:                              # pragma: no cover
        raise CheckFailed(f"本机没有 PyQt5，这条检查跑不了：{exc}") from None

    # QThread 需要一个 QCoreApplication 才肯好好跑；不起事件循环，只是建一个。
    app = QCoreApplication.instance() or QCoreApplication([])

    with fake_master.FakeMaster(publish_hz=200.0) as master:
        session = poller.CommandSession(
            f"tcp:127.0.0.1:{master.port}", deployment=None,
            repo_root=os.path.join(ROOT, "tools"), tick_hz=20.0, status_hz=10.0)
        try:
            session.start()
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                if master.snapshot_counters()["commands"] > 0:
                    break
                time.sleep(0.05)
            want(master.snapshot_counters()["commands"] > 0,
                 "会话起了 5 秒还没发出任何命令")
            # 先确认它真的占着，否则下面那句"释放后能用"什么也说明不了。
            want(_probe_command(master, timeout=1.0) is False,
                 "会话在跑，命令口却还能被别的客户端用——它根本没占住那一格")

            session.stop()
            want(session.wait(5000), "会话线程没在 5 秒内退出")
        finally:
            session.stop()
            session.wait(3000)

        # 进程还活着，这一次探测才有意义。
        want(_probe_command(master, timeout=2.0) is True,
             "释放之后命令口还是不通——那一格没还回去")
    del app


def _probe_command(master, timeout):
    """探一下命令口能不能用。

    返回 True（通，拿到了回复）/ False（进不去）。

    "进不去"在平台上有**两种长相**，都是同一件事：连接被 backlog 挡住。Linux 上连接
    会安静地排进队列、拿不到回复（超时）；Windows 上 backlog 满时直接发 RST，客户端
    拿到 ConnectionRefused。本机开发、台架在香橙派上跑，两种都会遇到，所以两种都算
    "被占着"。只认超时的话，在 Windows 上会把"占着"误判成"这一格没了"，报出来的错
    离原因很远。

    收到 EOF 是第三种情况，单独报出来——那是连接被对方关了，既不是回复也不是占住。
    """
    try:
        sock = socket.create_connection(("127.0.0.1", master.port), 3.0)
    except ConnectionRefusedError:
        return False
    sock.settimeout(timeout)
    try:
        sock.sendall(b"status\n")
        try:
            data = sock.recv(4096)
        except socket.timeout:
            return False
        want(bool(data), "命令口把连接关掉了（既不是回复也不是占住）")
        return True
    finally:
        sock.close()


def check_mode_guard_blocks_cst(context):
    """CST 部署要被挡在**连套接字之前**：位置量纲的目标发到力矩模式上是静默错发。"""
    with fake_master.FakeMaster(publish_hz=200.0) as master:
        args = [sys.executable, "-m", "emaster_gui",
                "--deployment", "orangepi-bench-cst-smoke",
                "--obs-endpoint", f"tcp:127.0.0.1:{master.obs_port}",
                "--selftest", "3", "--selftest-takeover", "--no-confirm"]
        done = subprocess.run(args, cwd=TOOLS, env=gui_env(), capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=120)
        want(done.returncode == 0, f"界面退出码 {done.returncode}：{done.stdout[-2000:]}")
        want(master.snapshot_counters()["commands"] == 0,
             "CST 部署下命令口还是被连上了——护栏没在连之前拦住")
        # 光拦住不够：**得让人看见为什么**。只拦不说，界面上是一片"主站状态未知"的
        # 空白轴表，操作员会以为界面坏了，反复点接管。所以要求那句话确实出现过。
        want("这个部署不能接管" in done.stdout,
             f"被拒了却没说理由：{done.stdout}")
        want("CST" in done.stdout and "已被拒绝" in done.stdout,
             f"拒绝的理由说得不清楚：{done.stdout}")
        want("轴表：没有快照" in done.stdout,
             f"被拒的会话却产出了快照：{done.stdout}")


# ---------------------------------------------------------------- 单元面

def check_affinity_is_quiet_off_linux(context):
    """非 Linux 上这一层必须**安静地什么都不做**，而不是抛异常把界面带下去。"""
    decision = rt_affinity._Decision(requested=None, log=lambda _m: None)
    result = rt_affinity.pin_current_thread(decision)
    if os.name == "nt" or not hasattr(os, "sched_setaffinity"):
        want(result is None, f"本机没有 sched_setaffinity，却返回了 {result}")
    else:
        want(result is not None, "Linux 上应当绑上核")
    # chosen 的说明要能读懂，不能说"没绑"却不给理由。
    cpus, note = rt_affinity.choose_cpus(None)
    if cpus is None:
        want(bool(note), "没绑核却没说为什么")


def check_reply_shapes(context):
    """回复前缀层数不一致，两条判据不能被双前缀骗过去。"""
    double = "OK|OK|Updated 2 external targets"
    want(emaster_client.reply_kind(double) == "ok", "双前缀 OK|OK| 没被认成成功")
    want(emaster_client.reply_text(double) == "Updated 2 external targets",
         f"双前缀没剥干净：{emaster_client.reply_text(double)!r}")
    want(emaster_client.reply_kind("ERROR|Command queue full") == "error",
         "单层 ERROR 没被认成失败")
    want(emaster_client.reply_kind("ERROR|ERROR|Not ready: state=0") == "error",
         "双前缀 ERROR 没被认成失败")
    # 按第一个竖线切会在这里出错：切出来是 "OK"，看着像成功。
    want(emaster_client.reply_kind("ERROR|ERROR|Not ready") != "ok",
         "双前缀 ERROR 被当成了成功")
    want(emaster_client.truncated("OK|state=4 axes=5|a1:pos=0|TRUNC"),
         "|TRUNC 没被认出来")


CHECKS = [
    ("默认只读：没点接管，命令口一个字节都不收", check_readonly_by_default),
    ("观测取数跟得上产出，且只连一次", check_observation_keeps_up),
    ("接管后真的动了、且单步没超限", check_takeover_moves_within_limits),
    ("曲线收到帧、抽了稀、只喂当前那根轴", check_charts_receive_and_decimate),
    ("窗口最小宽度放得进普通屏（急停按钮不能被挤出屏幕）", check_window_fits_on_a_normal_screen),
    ("接管时命令口那一格确实被占着", check_command_slot_is_held),
    ("释放时那一格确实还回去（同进程内探）", check_release_actually_releases),
    ("CST 部署被挡在连套接字之前", check_mode_guard_blocks_cst),
    ("CPU 亲和：非 Linux 上安静地不绑核", check_affinity_is_quiet_off_linux),
    ("回复判据不被双前缀骗过去", check_reply_shapes),
]

NOT_COVERED = [
    "**多线程之间的亲和隔离**：Linux 上 sched_setaffinity(0, ...) 只改调用线程、"
    "不改已在跑的其他线程（2026-09-18 在香橙派上实测过）。本机是 Windows，"
    "整条路都走不了，所以这条只在台架上成立与否由 scripts/bench_gui_e2e.sh 复核。",
    "**曲线画出来的形状**：这里只证它收到了帧、抽了稀、只喂当前那根轴。抽稀丢不丢"
    "包络由 scripts/checks/chart_check.py 负责（那边有隔点抽样做对照）；像素落在哪儿"
    "这边和那边都不证，只能靠台架上人眼对着轴的实际动作看。",
    "**被别的客户端顶掉之后的重连**：命令口被顶掉的表现是「挂住」不是报错，离线造"
    "这个场面要在探头里插一刀，与上面那条「占住」的检查是同一件事的两面，容易互相"
    "掩盖。台架上用真主站复现。",
    "**200 ms 外置目标看门狗不误触**：那是主站侧的判定，假主站没有这个模型，"
    "要真主站才测得到。",
    "**和主站抢不抢资源**：本机没有主站的实时线程可躲，A/B 只能在台架上做"
    "（scripts/bench_gui_e2e.sh 里那段）。",
]


def main():
    ap = argparse.ArgumentParser(description="图形界面离线回归（无头）")
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
        for item in NOT_COVERED:
            print(f"  - {item}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
