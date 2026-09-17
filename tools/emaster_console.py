#!/usr/bin/env python3
"""
EtherCAT 主站手动控制台（香橙派上的全屏面板）

用法：
    sudo bash scripts/console.sh [--start]
    python3 tools/emaster_console.py --deployment orangepi-bench-quint-30deg --start
    python3 tools/emaster_console.py --selftest --deployment ...   # 不开面板，跑一遍引擎

与主站解耦：本工具是一个**独立进程**，只通过命令套接字
/tmp/emaster-<部署>.sock 与主站通信（status / topology / set_external_target
/ halt / fault_reset）。主站不认识它、不依赖它，它挂了主站照常跑。
它可以把主站拉起来（--start 或按 m），但那只是 spawn 一个独立进程组的子进程，
不是把主站变成面板的一部分。

四条动手之前必须知道的事实（都是查过代码的）：

1. 主站对相邻两条目标有硬限幅 max_step（topology 里有）。超限不是拒绝这一条，
   而是判 MOTION_INVALID 后**中止整个会话**。所以目标只能一步步挪过去——
   本工具每 50ms 让目标朝期望值走一小步，永远不超限。
2. 超过 200ms 不发目标，主站转 HOLD：保持最后位置、仍停在 RUNNING、
   每拍继续重发同一个 607A。所以"松手就停"是天然的，不会掉使能。
3. stop/shutdown 是空操作，quick_stop 设的目标下一拍就被安全门改回
   OPERATION_ENABLED。**halt 1 是唯一粘性、不会被覆盖的暂停手段**（急停用它）。
   真停机只有 SIGINT（走安全门→全轴 SAFE_STOP→写报告），面板的 m 键发的是它。
4. 驱动器的软限位（0x607D）报的是 int32 全范围，等于没设；主站也不会因此拦。
   本工具的 ±范围 是**自己划的线**，不是驱动器的限制。
"""

import argparse
import contextlib
import io
import json
import math
import os
import select
import shutil
import signal
import subprocess
import sys
import time
import unicodedata

# termios/tty 是 Unix 专有的，Windows 上没有。面板只能在香橙派上跑，
# 但引擎和自检模式要能在任何机器上 import（本地也能验一遍逻辑）。
try:
    import termios
    import tty
except ImportError:  # pragma: no cover - 只在 Windows 上走到
    termios = None
    tty = None

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from interactive_control import EtherCATClient  # noqa: E402

# 目标下发频率。需远高于主站的 200ms 外部目标看门狗。
CONTROL_RATE_HZ = 50.0

# 面板刷新与状态轮询频率。一次 status 往返占主站一个周期槽，10Hz 很宽裕。
UI_RATE_HZ = 10.0

# 点动速度（输出轴度/秒）。实际每拍步长还受 max_step×0.8 约束，取小。
DEFAULT_JOG_SPEED = 10.0

# 点动速度档位（输出轴度/秒）。, / . 在档位之间走。
JOG_SPEED_STEPS = (1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0)

# 点动的时间参数。按一下就按设定速度走，不靠"按一次走一个步长 + 终端自动重复"
# ——那条路上的首次延迟（通常 0.5 秒）和重复频率（通常 30 次/秒）都不由我们控制，
# 手感是"按下去半天不动、动起来又一跳一跳"。改成按时间积分之后，速度档位才是真的。
JOG_GRACE_S = 0.12             # 这么久没有新的按键事件就当松手
JOG_RAMP_S = 0.30              # 按下后由慢加速到设定速度的时长
JOG_MIN_FRACTION = 0.15        # 刚开始时按设定速度的这个比例走（轻点 = 小位移）

# 单步不超过 max_step 的这个比例。留余量，避免取整刚好越界。
MAX_STEP_MARGIN = 0.8

# 断线重连的尝试间隔（秒）。别每拍都去 connect。
RECONNECT_INTERVAL_S = 1.0

# 主站刚被拉起来那段时间的重连间隔。主站的 accept 队列只有 1 格，半开连接要 5 秒
# 才回收（src/bus/soem/command_server.c:99-104），连接/断开快过这个节奏就会把后面的
# 连接全挤掉，客户端只会看到 Connection refused。所以这段窗口里慢一点、别重试。
STARTUP_RETRY_S = 3.0

# 自己的软范围默认值（度，相对启动位置）。0 表示不限制。
DEFAULT_RANGE_DEG = 45.0

# 按下停主站后等它自己走完停机流程的宽限时间（秒）。台架上停机序言加逐级撤使能
# 大约十几秒，给宽一点；超了才强杀。
STOP_GRACE_S = 45.0

# CiA402 状态字：掩码和几个要判的值（与 src/cia402/controller.c 同一张表）。
STATUS_MASK = 0x006F
STATUS_OPERATION_ENABLED = 0x0027
STATUS_QUICK_STOP_ACTIVE = 0x0007
STATUS_FAULT_REACTION = 0x000F
STATUS_FAULT = 0x0008
STATUS_BIT_INTERNAL_LIMIT = 0x0800

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
DIM = "\x1b[2m"
RED = "\x1b[31m"
GREEN = "\x1b[32m"
YELLOW = "\x1b[33m"
CLEAR_LINE = "\x1b[K"

# 全屏：面板画在备用屏幕缓冲区上，退出时终端把原来的内容原样还回去（历史不被擦）。
# 每行用绝对定位 \x1b[<行>;1H 落位，不用 \r\n 逐行推进——光标永远不越过屏幕底部，
# 物理上不可能顶屏滚动。
ALT_SCREEN_ON = "\x1b[?1049h"
ALT_SCREEN_OFF = "\x1b[?1049l"
HIDE_CURSOR = "\x1b[?25l"
SHOW_CURSOR = "\x1b[?25h"

HELP_TEXT = """
键位
  1-5      选轴（点动、单个数输入只作用于选中的这根）
  + / -    点动：按下就走、按住连续、松手停（方向键同效）   0  回启动位置
  g        走到角度。输入行就在面板里，打字时目标照发、控制不停
  s 数字   存住上一条 g 目标                      v 数字  取出来再走一遍
  , / .    调点动速度（1 到 100°/s）              [ / ]   调往返振幅（先按 t）
  t        往返验证：在启动位置与 +振幅 之间来回跑，面板记账
  q        急停（halt，使能不掉）    r 复位    z 把当前位置设成新零点
  m        启停主站（停时发 SIGINT，走正常停机并写报告）
  h        帮助                                   Q  退出面板（主站继续跑）

点动：按一下 ≈ 以设定速度走 0.1 秒，按住就走满这个速度，松手 0.1 秒内停。
  要更细就把速度调小（, 键），1°/s 时轻点一下不到 0.05°。
角度口径：以**启动位置**为零点（接上主站那一刻 = 0.00°），g 也按这个口径，
  输 30 = 走到启动位置 +30°。"圈内"是单圈读数，用来对驱动器自己的显示。
输入行三种写法（先按 g）
  30            选中轴走到 +30°
  30 12 5 0 0   按轴号顺序给全部 5 根
  1:30 4:12     只动轴1 和轴4，其余不动；多轴同时起停、同时到位
  ±范围 是自己划的保险（默认 ±45°，--range 改，0 关掉），不是驱动器的限制。
""".rstrip()


# ---------------------------------------------------------------- 客户端


class ConsoleClient(EtherCATClient):
    """把 interactive_control 的客户端改成不往 stdout 打印的版本。

    它原本在连接失败/通信失败时直接 print，那会糊在全屏面板上。这里把每次
    调用期间的 stdout 收进缓冲区，抽出最后一行留给面板显示。
    """

    def __init__(self, sock_path, timeout_s=1.0):
        super().__init__(sock_path)
        self.timeout_s = timeout_s
        self.last_problem = None
        self.last_response = None   # 最后一条原始回应，诊断用
        self.expect_startup = False  # 主站刚拉起来：每条命令只发一次（见 command）

    def connect(self):
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            ok = super().connect()
        noise = sink.getvalue().strip()
        if noise:
            self.last_problem = noise.splitlines()[-1]
        if ok:
            # 主站一条命令一个周期内就回，1 秒足够；缩短是为了主站没了的时候
            # 面板能立刻说出来，而不是像默认的 5 秒那样卡住。
            self.sock.settimeout(self.timeout_s)
        return ok

    def command(self, cmd, retries=2):
        # 主站刚被拉起来、还没答过话的这段时间：每条命令只发一次。基类失败时会
        # 断开重连再试，一秒里连两三回，正好把主站那只有一格的 accept 队列挤爆。
        if self.expect_startup and self.last_response is None:
            retries = 0
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            response = super().command(cmd, retries=retries)
        noise = sink.getvalue().strip()
        if noise:
            self.last_problem = noise.splitlines()[-1]
        self.last_response = response
        return response


# ---------------------------------------------------------------- 控制引擎


def find_config_document(repo_root, subdir, key, value):
    """在 config/<subdir> 下按 key == value 找一份配置原文；找不到返回 None。

    配置文件名的后缀是稳定的，前缀不是——部署里引用的是 ID，不是文件名。
    按 ID 去内容里找，改名不会让这里的对应关系失效。
    """
    directory = os.path.join(repo_root, "config", subdir)
    try:
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".json"):
                continue
            with open(os.path.join(directory, name), "r", encoding="utf-8") as handle:
                data = json.load(handle)
            if isinstance(data, dict) and data.get(key) == value:
                return data
    except (OSError, ValueError):
        return None
    return None


def deployment_selected_mode(repo_root, deployment_id):
    """部署选中的 CiA402 模式 ID（csp / csv / cst）；读不出来返回 None。

    走向与主站一致：部署 -> operation_profile_ids[0] -> selected_mode_id。
    一条部署只启用一根轴的方案时这是精确的；列了多份（不同设备各一份）时只看
    第一份——面板的护栏不追求精确到轴，只求"明显不是位置模式的部署别去发位置"。
    """
    deployment = find_config_document(repo_root, "deployments", "deployment_id", deployment_id)
    if not deployment:
        return None
    profile_ids = deployment.get("operation_profile_ids") or []
    if not profile_ids:
        return None
    profile = find_config_document(
        repo_root, "operation_profiles", "operation_profile_id", profile_ids[0])
    if not profile:
        return None
    mode = profile.get("selected_mode_id")
    return mode if isinstance(mode, str) else None


class Engine:
    """控制引擎：与界面无关，只跟套接字和两个数组打交道。

    每根轴两个数：
      desired[i]    —— 想让轴去哪儿（点动往上加，g 直接赋值）
      commanded[i]  —— 真正发给主站的最后一个值（接上时取主站的 planned）
    控制拍让 commanded 朝 desired 匀速趋近，每拍最多走 step_limit。
    点动、走到角度、急停后恢复，走的都是这一条路。
    """

    def __init__(self, client, deployment, jog_speed=DEFAULT_JOG_SPEED,
                 range_deg=DEFAULT_RANGE_DEG, repo_root=None):
        self.client = client
        self.deployment = deployment
        self.jog_speed = jog_speed
        self.range_deg = range_deg
        # 仓库根：用来读部署配置，判断这次跑的是不是位置模式。None 表示不查
        # （自检里造引擎的场合），此时护栏让开——它防的是真台架上的误操作。
        self.repo_root = repo_root
        self._selected_mode = None

        self.axis_count = 0
        self.factor = []           # 每轴：counts / 度
        self.origin = []           # 每轴：接上时的实际位置（counts），软范围零点
        # 零点只在**第一次**接上时锚定。掉线重连不重锚：否则连接一抖，"相对"那一栏
        # 和软范围就跟着漂。要换零点是显式动作（面板 z 键 / reanchor()）。
        self.origin_locked = False
        self.desired = []
        self.commanded = []
        # 本次运动还剩几拍走完。多轴共用同一个数，于是同时起停、同时到位。
        self.ticks_left = 0
        self.max_step = 0

        self.attached = False
        self.last_connect_try = 0.0
        # 主站是自己刚拉起来的吗？是的话重连要放慢（见 STARTUP_RETRY_S）。
        self.expect_startup = False
        self.halted = False
        self.selected_axis = 0
        self.message = "启动中"
        self.status = None
        self.axes = []
        self.master_state = -1
        self.sent_count = 0
        self.last_sent = None      # 最后一条真正发出去的目标，给自检核对用

    # ---------- 连接与初始化 ----------

    def mode_guard_passed(self):
        """面板只发位置目标，所以只接位置模式（CSP）的部署。

        非 CSP 部署直接拒绝，理由不是"还没做"，而是**目标会被当成别的东西**：
        同样是 set_external_target 写进去的整数，模式 9 当成速度、模式 10 当成
        额定力矩的千分比。面板里那套角度换算、软范围、单步挪动全是位置量纲的，
        换个模式它们只会安静地把错的目标发出去——安静正是最坏的一种错法。
        """
        if self.repo_root is None:
            return True
        if self._selected_mode is None:
            self._selected_mode = deployment_selected_mode(self.repo_root, self.deployment) or ""
        mode = self._selected_mode
        if mode == "" or mode == "csp":
            return True
        self.message = (f"部署 {self.deployment} 是 {mode.upper()} 模式（力矩/速度），"
                        f"面板发的是位置目标，已被拒绝。要用这个部署请走专用脚本。")
        return False

    def try_attach(self, now):
        """按节奏重试接上主站。单次尝试，不阻塞——面板要一直能按键。"""
        interval = STARTUP_RETRY_S if self.expect_startup else RECONNECT_INTERVAL_S
        if now - self.last_connect_try < interval:
            return False
        self.last_connect_try = now
        return self.attach()

    def expect_master_startup(self):
        """声明"主站是我刚拉起来的"：放慢重连、不发重试，直到它答上话为止。"""
        self.expect_startup = True
        self.client.expect_startup = True
        self.client.last_response = None    # 别拿上一任主站的回应当"已经答过话"

    def attach(self):
        """试一次：连接 → 要 RUNNING → 读拓扑 → 用主站已提交的目标当起点。"""
        if not self.mode_guard_passed():
            return False
        if self.client.sock is None and not self.client.connect():
            detail = f"（{self.client.last_problem}）" if self.client.last_problem else ""
            self.message = f"连不上套接字 {self.client.sock_path}{detail}"
            return False
        if not self.poll_status():
            raw = str(self.client.last_response or "").strip()
            self.message = (f"主站没回应（最近一条：{raw[:60]}）" if raw
                            else "套接字在，但主站没回应")
            self.client.disconnect()
            return False
        if self.master_state != 4:
            self.message = f"主站还在启动（state={self.master_state}），等它进 RUNNING…"
            return False

        if not self.client.refresh_topology():
            self.message = "读不到拓扑，角度换算用不了"
            return False

        self.axis_count = self.client.axis_count
        self.max_step = self.client.max_step
        self.factor = [self._axis_factor(i) for i in range(self.axis_count)]
        if not self.factor or any(f is None for f in self.factor):
            self.message = "拓扑里的编码器/减速比不完整，算不出角度"
            return False

        # 起点：优先用主站**已提交**的目标（planned），单步限幅正是拿它当基准；
        # 没有 planned 才退回实际位置。用错基准第一条命令就可能超限、打掉会话。
        if not self.origin_locked:
            self.origin = [self._axis_position(i) for i in range(self.axis_count)]
            self.origin_locked = True
        self.commanded = [self._axis_planned(i) for i in range(self.axis_count)]
        self.desired = list(self.commanded)
        self.ticks_left = 0
        self.attached = True
        self.halted = False
        self.message = "已接上主站，可以动手了"
        return True

    def wait_attached(self, timeout_s):
        """自检用：阻塞等到接上为止。节奏跟面板一致（别把主站的 accept 队列挤爆）。"""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self.try_attach(time.monotonic()):
                return True
            time.sleep(0.2)
        return False

    def _axis_factor(self, index):
        """该轴 1 度 = 多少 counts：enc × (gear_motor/gear_shaft) / 360。"""
        if not self.client.topology or index >= len(self.client.topology):
            return None
        axis = self.client.topology[index]
        try:
            increments = float(axis["enc"])
            gear_motor, gear_shaft = (float(x) for x in axis["gear"].split("/"))
        except (KeyError, ValueError):
            return None
        if gear_shaft == 0:
            return None
        return increments * (gear_motor / gear_shaft) / 360.0

    def _axis_field(self, index, name, fallback_name=None):
        if not self.axes or index >= len(self.axes):
            return 0
        axis = self.axes[index]
        raw = axis.get(name)
        if raw is None and fallback_name:
            raw = axis.get(fallback_name)
        # status / err 在协议里写的是 0x%04x（主站 session_control.c 就是这么打的），
        # 位置那几个字段是十进制。int(x) 认不了 0x 前缀，会让状态字一律读成 0，
        # 面板上就永远不会出现"故障/限位"——先按 0x 认，认不出再退回十进制。
        try:
            return int(raw, 0)
        except (TypeError, ValueError):
            pass
        try:
            return int(raw)
        except (TypeError, ValueError):
            return 0

    def _axis_position(self, index):
        return self._axis_field(index, "pos")

    def _axis_planned(self, index):
        return self._axis_field(index, "planned", fallback_name="pos")

    def counts_to_deg(self, index, counts):
        return counts / self.factor[index]

    def deg_to_counts(self, index, degrees):
        return int(round(degrees * self.factor[index]))

    def deployment_label(self):
        if self.axis_count:
            return f"部署 {self.deployment}（{self.axis_count} 轴）"
        return f"部署 {self.deployment}"

    # ---------- 状态 ----------

    def poll_status(self):
        status = self.client.refresh_status()
        # 没有 state= 的回应不是状态回应（协议里只有 set_external_target 之类会这样回）。
        # 当它有效的话 master_state 会变成 -1，后面全是无从下手的怪现象。
        if status is None or "state" not in status:
            self.status = None
            return False
        self.status = status
        self.axes = status["axes"]
        self.master_state = self.client.state
        self.expect_startup = False     # 主站答上话了，重连回到正常节奏
        return True

    def axis_status_word(self, index):
        return self._axis_field(index, "status")

    def axis_error_code(self, index):
        return self._axis_field(index, "err")

    def axis_state_label(self, index):
        """一轴一个词。顺序就是出事时你会想先看谁。"""
        if not self.axes or index >= len(self.axes):
            return ("离线", RED)
        word = self.axis_status_word(index)
        masked = word & STATUS_MASK
        if masked in (STATUS_FAULT, STATUS_FAULT_REACTION):
            return ("故障", RED)
        if word & STATUS_BIT_INTERNAL_LIMIT:
            return ("限位", RED)
        if masked == STATUS_QUICK_STOP_ACTIVE:
            return ("急停", YELLOW)
        if self.halted:
            return ("已停", YELLOW)
        if masked == STATUS_OPERATION_ENABLED:
            return ("就绪", GREEN)
        return ("未使能", YELLOW)

    def error_codes(self):
        """有非零错误码的轴：[(轴号, 码)]。"""
        return [(i, self.axis_error_code(i)) for i in range(len(self.axes))
                if self.axis_error_code(i) != 0]

    # ---------- 运动 ----------

    def step_limit_counts(self, index):
        """一拍允许的 counts 增量：点动速度与单步限幅取小，至少 1。"""
        by_speed = self.jog_speed * self.factor[index] / CONTROL_RATE_HZ
        limit = by_speed
        if self.max_step > 0:
            limit = min(by_speed, self.max_step * MAX_STEP_MARGIN)
        return max(1.0, limit)

    def range_limits(self, index):
        """软范围（counts）。range_deg<=0 时返回 None 表示不限制。"""
        if self.range_deg <= 0:
            return None
        span = self.range_deg * self.factor[index]
        return (self.origin[index] - span, self.origin[index] + span)

    def clamp_to_range(self, index, counts):
        """把目标收进软范围；返回 (值, 是否被夹住)。"""
        limits = self.range_limits(index)
        if limits is None:
            return counts, False
        low, high = limits
        if counts < low:
            return int(low), True
        if counts > high:
            return int(high), True
        return counts, False

    def in_range(self, index, counts):
        limits = self.range_limits(index)
        if limits is None:
            return True
        return limits[0] <= counts <= limits[1]

    def relative_degrees(self, index, counts):
        """把 counts 换算成"相对启动位置的角度"——面板上的角度口径就是这个。"""
        return (self.counts_to_deg(index, counts)
                - self.counts_to_deg(index, self.origin[index]))

    def range_headroom(self, index):
        """软范围还剩多少（度）：(还能往负走, 还能往正走)。没接上或关了范围返回 None。"""
        limits = self.range_limits(index)
        if limits is None or not self.axes:
            return None
        current = self._axis_planned(index)
        return (self.counts_to_deg(index, current - limits[0]),
                self.counts_to_deg(index, limits[1] - current))

    def reanchor(self):
        """把当前位置设成新的零点（软范围跟着移）。显式动作，重连不会自己发生。"""
        if not self.axes:
            self.message = "还没接上主站，没有可锚的位置"
            return False
        self.origin = [self._axis_position(i) for i in range(self.axis_count)]
        self.origin_locked = True
        self.message = "已把当前位置设成新的零点（软范围跟着移）"
        return True

    def _plan_move(self):
        """算"这次运动一共走几拍"，写进 ticks_left。

        多轴共用同一个拍数，于是各轴同时起停、同时到位；每拍增量由
        step_limit_counts 封顶，仍然不会碰到 max_step。
        """
        ticks = 0
        for index in range(self.axis_count):
            delta = abs(self.desired[index] - self.commanded[index])
            if delta == 0:
                continue
            ticks = max(ticks, int(math.ceil(delta / self.step_limit_counts(index))))
        self.ticks_left = ticks

    def nudge(self, index, sign, step_deg):
        """点动一步。被软范围夹住时在消息行说清楚。"""
        delta = self.deg_to_counts(index, step_deg) * sign
        if delta == 0:
            delta = sign
        target, clamped = self.clamp_to_range(index, self.desired[index] + delta)
        self.desired[index] = target
        self._plan_move()
        if clamped:
            edge = self.range_limits(index)[1 if sign > 0 else 0]
            direction = "正" if sign > 0 else "负"
            self.message = (f"轴{index + 1} 已到软范围{direction}向 "
                            f"{self.relative_degrees(index, edge):+.2f}°，到头了")
        else:
            self.message = (f"轴{index + 1} 目标 "
                            f"{self.relative_degrees(index, target):+.2f}°")

    def home_all(self):
        """全部轴回到启动位置。"""
        for index in range(self.axis_count):
            self.desired[index] = self.origin[index]
        self._plan_move()
        self.message = "回启动位置"

    def _commit_targets(self, targets):
        """targets: {轴号: 绝对度数}。先全部校验再落地：一轴越界，整条都不发。"""
        blocked = []
        counts = {}
        for index, degrees in targets.items():
            value = self.deg_to_counts(index, degrees)
            if not self.in_range(index, value):
                blocked.append(index)
                continue
            counts[index] = value

        if blocked:
            names = "、".join(f"轴{i + 1}" for i in sorted(blocked))
            self.message = (f"{names} 超出软范围 ±{self.range_deg:g}°"
                            f"（以启动位置为准），整条命令都没发")
            return False

        for index, value in counts.items():
            self.desired[index] = value
        self._plan_move()
        self.message = "开始走 " + " ".join(
            f"轴{i + 1} {self.relative_degrees(i, value):+.2f}°"
            for i, value in sorted(counts.items()))
        return True

    def goto_degrees(self, values):
        """走到**绝对**角度（自检用）。1 个数 = 选中轴，轴数个数 = 全轴。"""
        if len(values) == self.axis_count:
            pairs = list(enumerate(values))
        elif len(values) == 1:
            pairs = [(self.selected_axis, values[0])]
        else:
            self.message = (f"要么给 1 个数（选中轴），要么给 {self.axis_count} 个数")
            return False
        return self._commit_targets(dict(pairs))

    def goto_relative(self, pairs):
        """走到**相对启动位置**的角度（输入行的口径）。pairs: [(轴号, 度数)]。"""
        targets = {}
        for index, degrees in pairs:
            targets[index] = self.counts_to_deg(index, self.origin[index]) + degrees
        return self._commit_targets(targets)

    def halt_all(self):
        """急停：halt 1 + 冻结目标。粘性，不会被安全门回写覆盖。"""
        response = self.client.command("halt 1")
        for index in range(self.axis_count):
            self.desired[index] = self.commanded[index]
        self.ticks_left = 0
        self.halted = True
        if response and response.startswith("OK"):
            self.message = "已急停（halt 1），目标冻结；按 r 恢复"
        else:
            self.message = f"急停命令没有回音：{response}"

    def resume_all(self):
        """复位：先解除 halt，再发一次 fault_reset。"""
        first = self.client.command("halt 0")
        second = self.client.command("fault_reset")
        if self.master_state != 4:
            self.message = "会话已结束（主站不在 RUNNING），按 m 重启主站"
            return False
        self.halted = False
        ok = bool(first and first.startswith("OK")) and bool(second and second.startswith("OK"))
        self.message = ("已复位（halt 0 + fault_reset），可以继续点动" if ok
                        else f"复位命令回音异常：{first} / {second}")
        return ok

    def tick(self, now):
        """一个控制拍：commanded 朝 desired 走一步，然后把整组发出去。"""
        if not self.attached:
            self.try_attach(now)
            return
        if self.master_state != 4:
            # 会话不在 RUNNING 时主站会拒外部目标，别刷错误；等面板轮询把它带回来。
            return

        moved = False
        for index in range(self.axis_count):
            delta = self.desired[index] - self.commanded[index]
            if delta == 0:
                continue
            limit = int(self.step_limit_counts(index))
            if self.ticks_left > 0:
                # 本次运动还剩几拍：多轴共用同一个数，于是同时起停、同时到位。
                # 最后一拍（ticks_left==1）正好走完剩下的全部差值。
                step = int(round(abs(delta) / self.ticks_left))
            else:
                step = limit
            # 永远不许越过目标：超一点就会在目标两侧来回蹦（commanded 不收敛），
            # 而且会越过软范围那条自己划的线。
            step = max(1, min(step, limit, int(abs(delta))))
            self.commanded[index] += step if delta > 0 else -step
            moved = True
        if moved and self.ticks_left > 0:
            self.ticks_left -= 1

        positions = list(self.commanded)
        response = self.client.command(
            "set_external_target " + " ".join(str(p) for p in positions))
        if response is None or not response.startswith("OK"):
            # 发不出去就把账退回上一条真发出去的，别让本地账和主站账越差越远。
            if self.last_sent is not None:
                self.commanded = list(self.last_sent)
            self.message = f"目标没发出去：{response}"
            return
        self.sent_count += 1
        self.last_sent = positions
        if moved:
            self.message = "运动中"

    def detach(self):
        """断线：下一次控制拍会按节奏重新接上（会重算起点与软范围零点）。"""
        self.attached = False
        self.last_connect_try = 0.0

    def report_line(self):
        if not self.attached:
            return "未接上主站"
        return " ".join(
            f"轴{i + 1}={self.counts_to_deg(i, self.commanded[i]):+.2f}°"
            for i in range(self.axis_count))


# ---------------------------------------------------------------- 往返验证


# 往返验证的判据与节奏。
SOAK_ARRIVE_TOL_DEG = 0.20     # 到位判据：离端点这么近就算到了
SOAK_STALL_S = 2.0             # 命令走完还没到位，超过这么久记一次"到位超时"
SOAK_POLL_S = 0.1              # 自己轮询状态的节奏（10 Hz）
SOAK_LOG_PATH = "/tmp/emaster-console-soak.log"


class SoakRun:
    """往返验证：让一根轴在启动位置与"启动位置+振幅"之间来回跑，面板记账。

    它自己不管运动——只改 engine.desired，走的是和点动/goto 同一条控制拍，
    所以步长限幅、软范围、急停全都照样生效。

    记的全是**套接字能看见的事实**：会话结束、错误码、状态字故障位与限位、
    到位超时、状态轮询失败。同一句话 5 秒内只记一次，免得刷屏。异常同时追加
    到 /tmp/emaster-console-soak.log，事后可以对着主站报告看。
    """

    def __init__(self, engine, axis, amplitude_deg):
        self.engine = engine
        self.axis = axis
        self.amplitude = amplitude_deg
        self.active = False
        self.started_at = 0.0
        self.laps = 0
        self.arrivals = 0
        self.anomalies = []        # [(已跑秒数, 现象)]
        self.noted = {}            # 现象 -> 上次记的时间，用来去重
        self.direction = 1
        self.low = 0
        self.high = 0
        self.next_poll = 0.0
        self.at_end_since = None

    # ---------- 起停 ----------

    def start(self, now):
        engine = self.engine
        if not engine.axes or not engine.attached:
            engine.message = "还没接上主站，跑不了往返"
            return False
        self.low = engine.origin[self.axis]
        self.high = self.low + engine.deg_to_counts(self.axis, self.amplitude)
        if self.high == self.low:
            engine.message = "振幅太小（换算成 counts 是 0），把振幅调大一点"
            return False
        current = engine.commanded[self.axis]
        # 从离哪个端点近就先往另一个端点走，别一上来就穿整个行程。
        self.direction = 1 if abs(current - self.low) >= abs(current - self.high) else -1
        self.active = True
        self.started_at = now
        self.laps = 0
        self.arrivals = 0
        self.anomalies = []
        self.noted = {}
        self.at_end_since = None
        self.next_poll = 0.0
        self._aim()
        return True

    def stop(self):
        self.active = False
        summary = f"往返验证结束：{self.laps} 趟，异常 {len(self.anomalies)} 条"
        self.note_summary(summary)
        return summary

    def note_summary(self, summary):
        """停止时留一条汇总。异常是逐条追加的，一趟干净的往返本来什么都不落盘，
        事后想回看"跑了多久、多少趟"就没了依据。"""
        try:
            with open(SOAK_LOG_PATH, "a", encoding="utf-8") as handle:
                handle.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} "
                             f"轴{self.axis + 1} 振幅 {self.amplitude:g}° "
                             f"已跑 {self.elapsed_text(time.monotonic())} {summary}\n")
        except OSError:
            pass

    def set_amplitude(self, amplitude_deg):
        self.amplitude = amplitude_deg
        self.high = self.low + self.engine.deg_to_counts(self.axis, amplitude_deg)

    def _aim(self):
        """把目标指向当前方向的那个端点。"""
        engine = self.engine
        engine.desired[self.axis] = self.high if self.direction > 0 else self.low
        engine._plan_move()

    # ---------- 每拍 ----------

    def step(self, now):
        engine = self.engine
        if now >= self.next_poll:
            self.next_poll = now + SOAK_POLL_S
            if engine.attached and not engine.poll_status():
                engine.detach()
                self.note(now, "状态轮询失败（主站没回应），面板正在重连")
        self.watch(now)

        if not engine.axes or engine.master_state != 4 or engine.halted:
            return

        target = self.high if self.direction > 0 else self.low
        if engine.commanded[self.axis] != target:
            self.at_end_since = None
            return
        if self.at_end_since is None:
            self.at_end_since = now

        gap = abs(engine.counts_to_deg(
            self.axis, engine._axis_position(self.axis) - target))
        if gap <= SOAK_ARRIVE_TOL_DEG:
            self._flip(now, counted=True)
            return
        if now - self.at_end_since >= SOAK_STALL_S:
            self.note(now, f"到位超时：命令走完了，轴还差 {gap:.2f}° 没到")
            self._flip(now, counted=False)

    def _flip(self, now, counted):
        if counted:
            self.arrivals += 1
            if self.direction < 0:
                self.laps += 1
        self.direction = -self.direction
        self.at_end_since = now
        self._aim()
        self.engine.message = self.summary(now)

    # ---------- 记账 ----------

    def watch(self, now):
        engine = self.engine
        if engine.status is not None and engine.master_state != 4:
            self.note(now, f"主站会话结束（state={engine.master_state}）")
        for index in range(len(engine.axes)):
            code = engine.axis_error_code(index)
            if code:
                self.note(now, f"轴{index + 1} 错误码 0x{code:04x}")
            word = engine.axis_status_word(index)
            if word & STATUS_BIT_INTERNAL_LIMIT:
                self.note(now, f"轴{index + 1} 报限位（状态字 bit11）")
            if (word & STATUS_MASK) in (STATUS_FAULT, STATUS_FAULT_REACTION):
                self.note(now, f"轴{index + 1} 状态字是故障态（0x{word:04x}）")

    def note(self, now, text):
        previous = self.noted.get(text)
        if previous is not None and now - previous < 5.0:
            return
        self.noted[text] = now
        self.anomalies.append((now - self.started_at, text))
        try:
            with open(SOAK_LOG_PATH, "a", encoding="utf-8") as handle:
                handle.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} "
                             f"往返已跑 {self.elapsed_text(now)} "
                             f"轴{self.axis + 1} {text}\n")
        except OSError:
            pass

    def elapsed_text(self, now):
        seconds = max(0, int(now - self.started_at))
        return f"{seconds // 60}:{seconds % 60:02d}"

    def summary(self, now):
        return (f"往返验证 · 轴{self.axis + 1} 振幅 {self.amplitude:g}° · "
                f"已跑 {self.elapsed_text(now)} · 往返 {self.laps} 趟 · "
                f"异常 {len(self.anomalies)} 条")


# ---------------------------------------------------------------- 主站进程


class MasterProcess:
    """只管起停，不参与控制。主站永远是独立进程（start_new_session）。"""

    def __init__(self, repo_root, deployment, sock_path):
        self.repo_root = repo_root
        self.deployment = deployment
        self.sock_path = sock_path
        self.process = None
        self.log_path = "/tmp/emaster-console-master.log"

    @property
    def binary(self):
        return os.path.join(self.repo_root, "build", "tools", "master", "emaster-master")

    def socket_exists(self):
        return os.path.exists(self.sock_path)

    def running_pid(self):
        """我们自己拉起来的优先；否则在系统里找（别人起的也算）。"""
        if self.process is not None and self.process.poll() is None:
            return self.process.pid
        try:
            # -x 只比进程名，不比整条命令行。用 -f 的话，起主站的那条 shell 包装
            # （"bash -c '... emaster-master --deployment ..."）命令行里也带这个词，
            # 而且它的 pid 通常更小、会排在前面，面板就会把那个 shell 当成主站——
            # 停机提示于是让你去 kill 一个空壳，真主站还在跑。
            output = subprocess.run(["pgrep", "-x", "emaster-master"],
                                    capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            return None
        for line in output.stdout.split():
            if line.isdigit():
                return int(line)
        return None

    def start(self):
        # 拉起主站之前先看模式：非位置模式的部署不该由面板起——起来了也没法用
        # （引擎会拒绝接），只剩一个在跑的主站等着被误操作。
        mode = deployment_selected_mode(self.repo_root, self.deployment)
        if mode is not None and mode != "csp":
            return (f"部署 {self.deployment} 是 {mode.upper()} 模式，"
                    f"面板只发位置目标，拒绝启动主站")
        if not os.path.exists(self.binary):
            return f"没有主站二进制 {self.binary}（先在香橙派上 make）"
        if self.running_pid() is not None:
            return "主站已经在跑"
        handle = open(self.log_path, "wb")
        try:
            self.process = subprocess.Popen(
                [self.binary, "--deployment", self.deployment],
                cwd=self.repo_root, stdout=handle, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, start_new_session=True)
        except OSError as exc:
            return f"启动主站失败：{exc}"
        finally:
            handle.close()
        return None

    def request_stop(self):
        """发 SIGINT 就走，不等——停机要十几秒，等在这儿面板就不响应了。"""
        pid = self.running_pid()
        if pid is None:
            return "主站本来就没在跑", None
        try:
            os.kill(pid, signal.SIGINT)
        except OSError as exc:
            return f"发 SIGINT 失败：{exc}", None
        return None, pid

    def force_stop(self, pid):
        try:
            os.kill(pid, signal.SIGKILL)
            return f"主站 {pid} 没在宽限期内退出，已强杀（报告可能不完整）"
        except OSError as exc:
            return f"强杀失败：{exc}"

    def stop(self, timeout_s=30.0):
        """自检模式用：阻塞等到停稳。"""
        problem, pid = self.request_stop()
        if problem:
            return problem
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self.running_pid() is None:
                return "主站已正常停机，报告已写出"
            time.sleep(0.5)
        return self.force_stop(pid)

    def tail_log(self, lines=15):
        try:
            with open(self.log_path, "r", errors="replace") as handle:
                return "".join(handle.readlines()[-lines:])
        except OSError:
            return "(没有日志)"

    def report_path(self):
        """从部署配置里读 run_report_path，纯为了退出时告诉用户报告在哪。"""
        directory = os.path.join(self.repo_root, "config", "deployments")
        try:
            for name in sorted(os.listdir(directory)):
                if not name.endswith(".json"):
                    continue
                with open(os.path.join(directory, name), "r") as handle:
                    data = json.load(handle)
                if data.get("deployment_id") == self.deployment:
                    relative = data.get("run_report_path")
                    if relative:
                        return os.path.join(self.repo_root, relative)
        except (OSError, ValueError):
            pass
        return None


# ---------------------------------------------------------------- 面板


def char_width(char):
    """一个字符在终端里占几列。中日韩等全角字符占两列。"""
    if char == "\x1b":
        return 0
    if unicodedata.combining(char):
        return 0
    return 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1


def visible_width(text):
    """显示宽度：ANSI 序列不计入，全角字符算两列。"""
    width = 0
    index = 0
    while index < len(text):
        if text[index] == "\x1b":
            end = text.find("m", index)
            index = len(text) if end < 0 else end + 1
            continue
        width += char_width(text[index])
        index += 1
    return width


def pad(text, width):
    return text + " " * max(0, width - visible_width(text))


def truncate_ansi(text, width):
    """按显示宽度截断（全角算两列），保留其中完整的 ANSI 序列。

    这一段是"屏不滚"的最后一道保险：画出来的行只要有一列超宽，光标就会自动换行，
    换到最后一行的下一行就顶屏滚动。所以宁可少画一个全角字，也不许超。
    """
    out = []
    used = 0
    index = 0
    while index < len(text):
        if text[index] == "\x1b":
            end = text.find("m", index)
            if end < 0:
                break
            out.append(text[index:end + 1])
            index = end + 1
            continue
        size = char_width(text[index])
        if used + size > width:
            break
        out.append(text[index])
        used += size
        index += 1
    return "".join(out) + RESET


class Panel:
    def __init__(self, engine, master, args):
        self.engine = engine
        self.master = master
        self.args = args
        self.running = True
        self.fd = None
        self.saved_termios = None
        self.escape_buffer = b""
        self.escape_since = 0.0     # 落单 ESC 的到达时刻（用来判"按的就是 Esc"）
        self.jog = None             # [轴号, 方向, 按下时刻, 最后一次按键时刻]；None = 没在点动
        self.clock = time.monotonic  # 自检用：换掉它就能不用真等时间
        self.jog_speed_index = self._speed_index(engine.jog_speed)
        self.show_help = False
        self.notice = None
        self.notice_until = 0.0
        self.input_buffer = None    # 非 None = 输入行正在编辑
        self.pending_prefix = None  # 等第二个键的两键组合（s/v + 数字）
        self.last_targets = None    # 上一条 g 的目标，供姿态槽存
        self.poses = {1: None, 2: None, 3: None}
        self.soak = None            # 往返验证（SoakRun），没跑过是 None
        self.next_control = 0.0
        self.next_status = 0.0
        self.next_draw = 0.0
        self.next_master_check = 0.0
        self.stop_pid = None
        self.stop_deadline = 0.0

    @staticmethod
    def _speed_index(speed):
        for index, value in enumerate(JOG_SPEED_STEPS):
            if value >= speed:
                return index
        return len(JOG_SPEED_STEPS) - 1

    # ---- 终端 ----

    def enter_terminal(self):
        self.fd = sys.stdin.fileno()
        self.saved_termios = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        # 进备用屏幕缓冲区：面板有自己的屏，退出时终端把原来的内容原样还回去，
        # 用户的历史和滚动位置都不动。绘制全靠绝对定位（见 draw），不靠换行。
        sys.stdout.write(ALT_SCREEN_ON + HIDE_CURSOR + "\x1b[2J")
        sys.stdout.flush()

    def leave_terminal(self):
        if self.saved_termios is not None and termios is not None:
            try:
                termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved_termios)
            except termios.error:
                pass
            self.saved_termios = None
        sys.stdout.write(SHOW_CURSOR + ALT_SCREEN_OFF)
        sys.stdout.flush()

    def set_notice(self, text, seconds=4.0):
        self.notice = text
        self.notice_until = time.monotonic() + seconds

    # ---- 按键 ----

    def read_keys(self, timeout_s):
        keys = []
        # 落单的 ESC：用户按的就是 Esc（不是方向键）。单独一个 \x1b 进来后等一小会儿，
        # 没有后续字节就认它，免得方向键的第一字节被当成 Esc。
        if (self.escape_buffer == b"\x1b" and self.escape_since
                and time.monotonic() - self.escape_since > 0.05):
            self.escape_buffer = b""
            self.escape_since = 0.0
            keys.append("ESC")

        ready, _, _ = select.select([sys.stdin], [], [], timeout_s)
        if not ready:
            return keys
        data = os.read(self.fd, 64)
        if not data:
            return keys + ["Q"]  # 标准输入关了：按退出处理
        self.escape_buffer += data
        buffer = self.escape_buffer
        self.escape_buffer = b""
        index = 0
        while index < len(buffer):
            byte = buffer[index:index + 1]
            if byte != b"\x1b":
                keys.append(byte.decode("utf-8", "replace"))
                index += 1
                continue
            # 转义序列：只认方向键，其余（Home/PageUp 之类）整段丢掉。
            rest = buffer[index:]
            if len(rest) < 3:
                self.escape_buffer = rest
                if len(rest) == 1:
                    self.escape_since = time.monotonic()
                break
            if rest[1:2] == b"[" and rest[2:3] in (b"A", b"B", b"C", b"D"):
                keys.append({b"A": "+", b"B": "-", b"C": "]", b"D": "["}[rest[2:3]])
                index += 3
                continue
            index += 1
        return keys

    def step_jog(self, now):
        """点动的一拍：按下就走，按键连着来就一直走，停了就自己收住。

        按时间积分，而不是"按一次走一个固定步长"：后者要靠终端自动重复，
        首次重复前有半秒左右的空档（按下去没反应），之后又按重复频率一跳一跳。
        这里按下先按 JOG_MIN_FRACTION 慢慢起步（轻点 = 小位移），按住 0.3 秒
        加到设定速度；松手 = 不再有按键事件，速度在一个 JOG_GRACE_S 里线性收到 0，
        不会戛然而止也不会冲出去。
        """
        jog = self.jog
        if jog is None:
            return
        axis, sign, pressed_at, last = jog
        idle = now - last
        if idle > JOG_GRACE_S:
            self.jog = None
            return
        engine = self.engine
        ramp = min(1.0, JOG_MIN_FRACTION
                   + (1.0 - JOG_MIN_FRACTION) * (now - pressed_at) / JOG_RAMP_S)
        taper = max(0.0, 1.0 - idle / JOG_GRACE_S)
        step_deg = engine.jog_speed * ramp * taper / CONTROL_RATE_HZ
        engine.nudge(axis, sign, step_deg)

    def handle_key(self, key):
        # 输入行和前缀状态先吃按键，别让它们落到运动键上。
        if self.input_buffer is not None:
            self.handle_input_key(key)
            return
        if self.pending_prefix is not None:
            self.handle_prefix_key(key)
            return

        # 按了别的键就当松手：别让轴在用户已经开始干别的事之后接着往前跑。
        if key not in ("+", "-") and self.jog is not None:
            self.jog = None

        engine = self.engine
        if key in ("Q", "\x03"):
            self.running = False
            return
        if self.show_help:
            self.show_help = False
            return
        if key in "123456789" and engine.axis_count:
            index = int(key) - 1
            if index < engine.axis_count:
                engine.selected_axis = index
            return
        if key in ("+", "-"):
            self.stop_soak("手动点动")
            now = self.clock()
            sign = 1 if key == "+" else -1
            # 同一根轴同一个方向连着来 = 还按着，续上别重新加速；换了方向/轴就重来。
            if self.jog and self.jog[0] == engine.selected_axis and self.jog[1] == sign:
                self.jog[3] = now
            else:
                self.jog = [engine.selected_axis, sign, now, now]
            return
        if key == "0":
            self.stop_soak("手动回起点")
            engine.home_all()
            return
        if key in ("[", "]"):
            # 点动的"走多远"由速度和时间决定（按一下 ≈ 速度 × 0.1 秒），所以这里
            # 只剩往返振幅。手速快慢才是点动的细/粗，改速度用 , / 。
            if self.soak and self.soak.active:
                self.adjust_amplitude(-0.5 if key == "[" else 0.5)
            else:
                self.set_notice("方括号是往返振幅（先按 t）；点动的粗细用 , / . 调速度", 4)
            return
        if key in (",", "."):
            self.adjust_speed(1 if key == "." else -1)
            return
        if key == "g":
            self.open_input()
            return
        if key in ("s", "v"):
            self.pending_prefix = key
            self.set_notice("存这一条目标：按 1-3" if key == "s"
                            else "取姿态槽：按 1-3（Esc 取消）", 5)
            return
        if key == "t":
            self.toggle_soak()
            return
        if key == "z":
            engine.reanchor()
            return
        if key == "q":
            self.stop_soak("急停")
            engine.halt_all()
            return
        if key == "r":
            engine.resume_all()
            return
        if key == "m":
            self.toggle_master()
            return
        if key == "h":
            self.show_help = True
            return

    def adjust_speed(self, direction):
        self.jog_speed_index = max(0, min(len(JOG_SPEED_STEPS) - 1,
                                          self.jog_speed_index + direction))
        self.engine.jog_speed = JOG_SPEED_STEPS[self.jog_speed_index]
        self.set_notice(f"点动速度 {self.engine.jog_speed:g}°/s", 3)

    # ---- 输入行（面板内编辑，不进阻塞 input()）----

    def open_input(self):
        if not self.engine.axes:
            self.set_notice("还没接上主站，等接上再输角度", 4)
            return
        self.input_buffer = ""
        self.notice = None

    def handle_input_key(self, key):
        if key in ("\r", "\n"):
            text = self.input_buffer
            self.input_buffer = None
            self.submit_input(text)
            return
        if key in ("\x03", "ESC"):
            self.input_buffer = None
            self.set_notice("已取消输入", 2)
            return
        if key in ("\x7f", "\x08"):
            self.input_buffer = self.input_buffer[:-1]
            return
        if key == "\x15":  # Ctrl-U 清空这一行
            self.input_buffer = ""
            return
        if key.isprintable() and key != "\t":
            self.input_buffer += key

    def submit_input(self, text):
        engine = self.engine
        try:
            targets = self.parse_targets(text)
        except ValueError:
            self.set_notice("没看懂。写法：30 ｜ 30 12 5 0 0 ｜ 1:30 4:12", 6)
            return
        if not targets:
            return
        self.stop_soak("手输了目标")
        if engine.goto_relative(targets):
            self.last_targets = targets

    def parse_targets(self, text):
        """输入行 → [(轴号, 相对启动位置的度数)]。三种写法见 HELP_TEXT。"""
        engine = self.engine
        tokens = text.replace(",", " ").replace("，", " ").split()
        if not tokens:
            return []
        if any(":" in token for token in tokens):
            pairs = []
            for token in tokens:
                axis_text, sep, value_text = token.partition(":")
                if not sep or not axis_text or not value_text:
                    raise ValueError(token)
                index = int(axis_text) - 1
                if not 0 <= index < engine.axis_count:
                    raise ValueError(token)
                pairs.append((index, float(value_text)))
            return pairs
        if len(tokens) == 1:
            return [(engine.selected_axis, float(tokens[0]))]
        if len(tokens) == engine.axis_count:
            return [(index, float(value)) for index, value in enumerate(tokens)]
        raise ValueError(text)

    def handle_prefix_key(self, key):
        prefix = self.pending_prefix
        self.pending_prefix = None
        if key not in "123":
            self.set_notice("姿态槽只有 1/2/3", 3)
            return
        slot = int(key)
        if prefix == "s":
            if not self.last_targets:
                self.set_notice("还没有目标可存，先按 g 输一条", 5)
                return
            self.poses[slot] = list(self.last_targets)
            self.set_notice(f"已存进姿态槽 {slot}：{self.pose_text(slot)}", 5)
            return
        if self.poses[slot] is None:
            self.set_notice(f"姿态槽 {slot} 是空的", 4)
            return
        self.stop_soak("取姿态")
        if self.engine.goto_relative(self.poses[slot]):
            self.last_targets = list(self.poses[slot])
            self.set_notice(f"走姿态槽 {slot}：{self.pose_text(slot)}", 4)

    def pose_text(self, slot):
        pose = self.poses.get(slot)
        if not pose:
            return "空"
        return "  ".join(f"轴{index + 1} {value:+.2f}°" for index, value in pose)

    def adjust_amplitude(self, delta):
        soak = self.soak
        limit = self.engine.range_deg if self.engine.range_deg > 0 else 180.0
        amplitude = max(0.1, min(limit, soak.amplitude + delta))
        soak.set_amplitude(amplitude)
        self.set_notice(f"往返振幅 {amplitude:g}°（跑完这一趟生效）", 3)

    def toggle_soak(self):
        engine = self.engine
        if self.soak and self.soak.active:
            self.set_notice(self.soak.stop(), 6)
            return
        if not engine.axes or engine.master_state != 4:
            self.set_notice("主站不在 RUNNING，跑不了往返验证", 5)
            return
        amplitude = self.soak.amplitude if self.soak else self.default_amplitude()
        soak = SoakRun(engine, engine.selected_axis, amplitude)
        if not soak.start(time.monotonic()):
            self.set_notice(engine.message, 5)
            return
        self.soak = soak
        self.set_notice(f"往返验证开始：{soak.summary(time.monotonic())}", 8)

    def default_amplitude(self):
        limit = self.engine.range_deg if self.engine.range_deg > 0 else 180.0
        return round(min(30.0, limit * 0.6), 2)

    def stop_soak(self, why):
        if self.soak and self.soak.active:
            self.set_notice(f"{self.soak.stop()}（{why}）", 6)

    def toggle_master(self):
        pid = self.master.running_pid()
        if pid is None:
            problem = self.master.start()
            if problem:
                self.set_notice(problem, 8)
                return
            self.set_notice("主站已拉起，等它进 RUNNING…（初始化要几十秒）", 8)
            self.engine.detach()
            # 从建套接字到能应答之间主站在忙 SDO 初始化，这段窗口里慢点重连。
            self.engine.expect_master_startup()
            return
        # 自己拉起来的随时能停；别人起的（或没记清谁起的）要显式给 --allow-stop-master，
        # 免得一个按键把别人正在跑的东西停了。
        our_child = (self.master.process is not None
                     and self.master.process.poll() is None
                     and self.master.process.pid == pid)
        if not (self.args.allow_stop_master or our_child):
            self.set_notice(f"主站在跑（pid {pid}）。它不是本面板起的，"
                            f"要停就 kill -INT {pid}（或用 --allow-stop-master 打开这个键）", 8)
            return
        problem, stopped_pid = self.master.request_stop()
        if problem:
            self.set_notice(problem, 8)
            return
        self.stop_pid = stopped_pid
        self.stop_deadline = time.monotonic() + STOP_GRACE_S
        self.set_notice(f"已发 SIGINT（pid {stopped_pid}），等它走完停机流程…", STOP_GRACE_S)
        self.engine.detach()
        self.engine.status = None
        self.engine.message = "正在停主站"

    # ---- 绘制 ----

    def build_frame(self, width, height):
        """返回一行行文本；每行带一个优先级，装不下时从 3 往上丢（见 _fit）。"""
        engine = self.engine
        if self.show_help:
            rows = [(0, BOLD + " 帮助（按任意键返回）" + RESET)]
            rows += [(0, " " + line) for line in HELP_TEXT.splitlines()]
            return self._fit(rows, height)

        bar = (3, DIM + " " + "─" * max(10, width - 2) + RESET)
        rows = []
        if engine.master_state == 4:
            head = f" 主站 {GREEN}运行中{RESET}  第 {self._cycle()} 拍"
        elif engine.status is None:
            head = f" 主站 {RED}没在回应{RESET}"
        else:
            head = (f" 主站 {YELLOW}已停止{RESET}"
                    f"（state={engine.master_state}，会话结束，报告已写出）")
        span = f"软范围 ±{engine.range_deg:g}°（自己划的）" if engine.range_deg > 0 else "软范围 关"
        rows.append((0, head + f"    {span}    {engine.deployment_label()}"))
        rows.append(bar)

        if engine.axis_count == 0:
            rows.append((0, " 还没接上主站。" + (engine.message or "")))
            rows.append((1, f" 套接字 {engine.client.sock_path}"))
            rows.append((1, " 按 m 拉起主站；主站已经起了的话，它自己会接上去。"))
        else:
            rows += [(0, line) for line in self._axis_table(width)]

        codes = engine.error_codes()
        if codes:
            rows.append((2, " " + RED + "错误码 "
                         + " ".join(f"轴{i + 1}=0x{code:04x}" for i, code in codes) + RESET))

        rows.append(bar)
        if self.input_buffer is not None:
            rows.append((0, " " + BOLD + "> " + RESET + self.input_buffer + "▏"
                            + DIM + "  回车走 / Esc 取消；写法 30 ｜ 30 12 5 0 0 ｜ 1:30 4:12" + RESET))
        elif self.notice and time.monotonic() < self.notice_until:
            rows.append((1, " " + YELLOW + self.notice + RESET))
        elif self.soak and self.soak.active:
            rows.append((1, " " + GREEN + self.soak.summary(time.monotonic()) + RESET))
        else:
            self.notice = None
            rows.append((1, " " + (engine.message or "")))
        if self.soak and self.soak.anomalies:
            stamp, text = self.soak.anomalies[-1]
            rows.append((2, " " + RED + "最近一条异常（跑到 "
                             f"{int(stamp // 60)}:{int(stamp % 60):02d}）：{text}" + RESET))
        running = GREEN + "点动中" + RESET if self.jog else ""
        rows.append((2, f" 选中 {BOLD}轴{engine.selected_axis + 1}{RESET}"
                        f"   点动 {engine.jog_speed:g}°/s {running}"
                        + self._headroom_text()
                        + f"   已发 {engine.sent_count} 条"))
        rows.append((3, " [1-5]选轴 [+/-]点动（按住连续） [0]回起点 [g]走到角度 [,/.]调速"))
        rows.append((3, " [t]往返验证 [q]急停 [r]复位 [z]重锚零点 [h]帮助 [Q]退出"))
        rows.append((3, " [s]存姿态 [v]取姿态 都要接数字 1-3；[m]启停主站"))
        if engine.axis_count and engine.master_state != 4:
            rows.append((1, " " + RED + "会话已结束——按 m 重启主站" + RESET))
        return self._fit(rows, height)

    @staticmethod
    def _fit(rows, height):
        """按优先级塞进 height 行：0 必留，3 最先丢。"""
        for priority in (3, 2, 1):
            if len(rows) <= height:
                break
            rows = [row for row in rows if row[0] != priority]
        return [line for _, line in rows][:height]

    def _cycle(self):
        if not self.engine.status:
            return "?"
        return self.engine.status.get("cycle", "?")

    def _headroom_text(self):
        """选中轴的软范围余量：还能往正走 / 还能往负走。"""
        engine = self.engine
        if not engine.axes:
            return ""
        headroom = engine.range_headroom(engine.selected_axis)
        if headroom is None:
            return ""
        back, forward = headroom
        return f"   还能 {forward:+.1f}° / {-back:+.1f}°"

    def _axis_table(self, width):
        """一根轴一列。角度一律以"启动位置"为零点，g 输入也是这个口径。"""
        engine = self.engine
        titles = ["", "相对", "圈内", "目标", "偏差", "状态"]
        columns = []
        for index in range(engine.axis_count):
            if columns and 10 * (len(columns) + 1) + 5 > width:
                break
            actual_counts = engine._axis_position(index)
            target_counts = engine._axis_planned(index)
            relative = engine.relative_degrees(index, actual_counts)
            target = engine.relative_degrees(index, target_counts)
            inside = engine.counts_to_deg(index, actual_counts) % 360.0
            label, colour = engine.axis_state_label(index)
            columns.append([
                f"轴{index + 1}",
                f"{relative:+8.2f}°",
                f"{inside:7.1f}°",
                f"{target:+8.2f}°",
                f"{target - relative:+8.2f}°",
                f"{colour}●{label}{RESET}",
            ])
        lines = []
        for row, title in enumerate(titles):
            text = " " + pad(title, 5)
            for column in columns:
                text += " " + pad(column[row], 9)
            lines.append(text)
        if len(columns) < engine.axis_count:
            lines.append(f" （终端太窄，还有 {engine.axis_count - len(columns)} 根轴没显示）")
        return lines

    def draw(self, width, height):
        """绝对定位逐行画，不用换行——光标从不下移，所以屏永远不滚。"""
        if width < 20 or height < 6:
            lines = [" 终端太小：面板至少要 20 列 × 6 行"]
        else:
            lines = self.build_frame(width, height)
        out = []
        for row, line in enumerate(lines, start=1):
            out.append(f"\x1b[{row};1H" + truncate_ansi(line, width) + CLEAR_LINE)
        out.append(f"\x1b[{len(lines) + 1};1H\x1b[J")  # 光标停在最后一行之后，清掉残留
        sys.stdout.write("".join(out))
        sys.stdout.flush()

    # ---- 事件循环 ----

    def run(self):
        engine = self.engine
        control_period = 1.0 / CONTROL_RATE_HZ
        ui_period = 1.0 / UI_RATE_HZ
        now = time.monotonic()
        self.next_control = now
        self.next_status = now
        self.next_draw = now
        self.next_master_check = now
        self.enter_terminal()
        try:
            while self.running:
                now = time.monotonic()

                if now >= self.next_control:
                    self.next_control += control_period
                    if self.next_control <= now:
                        self.next_control = now + control_period
                    if self.soak and self.soak.active:
                        self.soak.step(now)   # 先摆目标，再发这一拍
                    self.step_jog(now)        # 点动也走同一拍
                    engine.tick(now)

                if now >= self.next_status:
                    self.next_status = now + ui_period
                    if self.soak and self.soak.active:
                        pass  # 往返模式自己按 SOAK_POLL_S 轮询状态，这里不重复问
                    elif engine.attached and not engine.poll_status():
                        engine.detach()
                        self.set_notice("主站没回应了，正在重连…", 5)

                if now >= self.next_master_check:
                    self.next_master_check = now + 2.0
                    self.check_master_gone()

                if now >= self.next_draw:
                    self.next_draw = now + ui_period
                    size = shutil.get_terminal_size((100, 30))
                    self.draw(size.columns, max(6, size.lines - 1))

                wait = min(self.next_control, self.next_status,
                           self.next_draw, self.next_master_check) - time.monotonic()
                for key in self.read_keys(max(0.0, min(wait, 0.05))):
                    self.handle_key(key)
        finally:
            self.leave_terminal()

    def check_master_gone(self):
        """每两秒一次：停机宽限期到了就强杀；主站没了就把面板上的话说对。"""
        if self.stop_pid is not None:
            if self.master.running_pid() is None:
                self.set_notice("主站已停机，报告已写出", 8)
                self.stop_pid = None
            elif time.monotonic() > self.stop_deadline:
                self.set_notice(self.master.force_stop(self.stop_pid), 8)
                self.stop_pid = None
            return
        if self.engine.attached:
            return
        if self.master.running_pid() is None and self.engine.status is not None:
            self.engine.status = None
            self.engine.message = "主站进程没了"


# ---------------------------------------------------------------- 自检


def selftest(args, client_factory=ConsoleClient):
    """不开面板跑一遍引擎：接主站 → 走一段再回来 → 急停 → 复位 → 越界拦截。

    全屏交互没法在 ssh 里自动验，这个模式是控制台在台架上能真跑一遍的那条路。
    client_factory 是留给离线自测的接缝：换掉客户端就能对着假主站跑同一套断言。
    """
    sock_path = args.socket or f"/tmp/emaster-{args.deployment}.sock"
    master = MasterProcess(args.repo, args.deployment, sock_path)
    started_here = False
    if not master.socket_exists():
        if not args.start:
            print(f"错误：套接字不存在（{sock_path}）。先起主站，或加 --start。")
            return 1
        problem = master.start()
        if problem:
            print("错误：" + problem)
            return 1
        started_here = True
        print("已拉起主站，等套接字…")
        for _ in range(120):
            if master.socket_exists():
                break
            time.sleep(0.5)
        else:
            print("错误：主站没建出套接字。日志尾巴：")
            print(master.tail_log())
            return 1

    client = client_factory(sock_path)
    engine = Engine(client, args.deployment, args.jog_speed, args.range,
                    repo_root=args.repo)
    if started_here:
        # 主站从建套接字到能应答之间要跑完几十秒的 SDO 初始化，这段时间别去挤它。
        engine.expect_master_startup()
    print(f"接主站：{sock_path}")
    if not engine.wait_attached(180.0 if started_here else 60.0):
        print("错误：" + engine.message)
        return 1
    print(f"{engine.deployment_label()}  单步限幅 {engine.max_step} counts"
          f" ≈ {engine.counts_to_deg(0, engine.max_step):.2f}°")
    print("起点（度）：" + " ".join(
        f"轴{i + 1}={engine.counts_to_deg(i, engine.commanded[i]):+.3f}°"
        for i in range(engine.axis_count)))

    failures = []
    sent = []

    def drive(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            engine.tick(time.monotonic())
            if engine.last_sent is not None and (
                    not sent or sent[-1] != list(engine.last_sent)):
                sent.append(list(engine.last_sent))
            time.sleep(1.0 / CONTROL_RATE_HZ)
        engine.poll_status()

    def check(label, condition, detail=""):
        print(f"  [{'通过' if condition else '失败'}] {label} {detail}".rstrip())
        if not condition:
            failures.append(label)

    # 1) 走一段再回起点
    travel = args.travel
    print(f"\n1) 斜坡走 {travel:g}° 再回起点（约 {travel / max(engine.jog_speed, 0.1) * 2 + 2:.0f} 秒）")
    start_state = list(engine.commanded)
    # g 收的是**绝对**角度（面板上"角度/目标"两栏就是绝对量），台架上的绝对值是
    # 470° 这种，所以得从起点换算，不能直接发 1.0——那样会被软范围挡掉，而"根本没动"
    # 也能让下面的回位断言通过，等于什么都没验。
    origin_deg = [engine.counts_to_deg(i, start_state[i]) for i in range(engine.axis_count)]
    engine.goto_degrees([origin_deg[0] + travel])
    drive(travel / max(engine.jog_speed, 0.1) * 2.0 + 1.5)
    engine.poll_status()
    moved = engine.counts_to_deg(0, engine._axis_position(0)) - origin_deg[0]
    check("轴1 确实走了", abs(moved) > travel * 0.5, f"走了 {moved:+.3f}°")
    print("  到达：" + engine.report_line())
    engine.goto_degrees([origin_deg[0]])
    drive(travel / max(engine.jog_speed, 0.1) * 2.0 + 1.5)
    print("  回位：" + engine.report_line())

    engine.poll_status()
    for index in range(engine.axis_count):
        expected = engine.counts_to_deg(index, start_state[index])
        actual = engine.counts_to_deg(index, engine._axis_position(index))
        check(f"轴{index + 1}回到起点附近", abs(actual - expected) < 0.5,
              f"实际 {actual:+.3f}° 起点 {expected:+.3f}°")

    # 2) 发出去的目标序列，相邻增量不能超限
    print("\n2) 目标序列的相邻增量")
    worst = 0
    for index in range(engine.axis_count):
        previous = start_state[index]
        for positions in sent:
            worst = max(worst, abs(positions[index] - previous))
            previous = positions[index]
    if engine.max_step > 0:
        allowed = engine.max_step * MAX_STEP_MARGIN
        check("最大相邻增量 ≤ max_step×0.8", worst <= allowed,
              f"实测 {worst} counts，上限 {allowed:.0f}")
    else:
        check("单步限幅未启用，跳过", True)
    check("确实发出过目标", len(sent) > 5, f"{len(sent)} 条")

    # 3) 状态字认不认得出来：协议里是 0x%04x，解析失手会一律读成 0，
    #    面板上就永远看不到"故障/限位"。
    print("\n3) 状态字解析")
    words = [engine.axis_status_word(i) for i in range(engine.axis_count)]
    check("状态字不是全 0", any(word != 0 for word in words),
          " ".join(f"轴{i + 1}=0x{word:04x}" for i, word in enumerate(words)))
    labels = [engine.axis_state_label(i)[0] for i in range(engine.axis_count)]
    check("没有哪根轴被误判成未使能", all(label != "未使能" for label in labels),
          " ".join(labels))

    # 4) 急停与复位
    print("\n4) 急停与复位")
    engine.halt_all()
    time.sleep(1.0)
    engine.poll_status()
    states = [engine.axis_state_label(i)[0] for i in range(engine.axis_count)]
    check("主站仍在 RUNNING", engine.master_state == 4, f"state={engine.master_state}")
    check("面板显示已停", all(s == "已停" for s in states), " ".join(states))
    engine.resume_all()
    time.sleep(0.5)
    print("  复位后：" + (engine.message or ""))

    # 5) 软范围拦截
    if engine.range_deg > 0:
        print(f"\n5) 软范围 ±{engine.range_deg:g}° 拦截")
        engine.poll_status()
        engine.goto_degrees([engine.range_deg * 3])
        check("越界目标被拒", "软范围" in (engine.message or ""), engine.message)
        check("主站未受影响", engine.master_state == 4)

    # 6) 输入行解析、相对口径、面板绘制（都不动电机）
    print("\n6) 输入行解析 / 相对口径 / 面板绘制")
    panel = Panel(engine, master, args)
    pairs = panel.parse_targets("1:30 4:12")
    check("1:30 4:12 = 只动轴1和轴4", pairs == [(0, 30.0), (3, 12.0)], str(pairs))
    pairs = panel.parse_targets("30")
    check("单个数 = 当前选中轴", pairs == [(engine.selected_axis, 30.0)], str(pairs))
    full = " ".join(str((i + 1) * 10) for i in range(engine.axis_count))
    pairs = panel.parse_targets(full)
    check(f"{engine.axis_count} 个数 = 按轴号顺序给全轴",
          pairs == [(i, (i + 1) * 10.0) for i in range(engine.axis_count)], full)
    check("空行 = 什么都不做", panel.parse_targets("   ") == [])
    bad_inputs = ["abc", "1:30 9:1", "1:2:3"]
    if engine.axis_count != 2:
        bad_inputs.append("30 12")     # 个数既不是 1 也不是全轴
    for bad in bad_inputs:
        try:
            panel.parse_targets(bad)
            check(f"非法写法 {bad!r} 被拒", False)
        except ValueError:
            check(f"非法写法 {bad!r} 被拒", True)

    engine.goto_relative([(0, 0.5)])
    target = engine.relative_degrees(0, engine.desired[0])
    check("g 0.5 = 走到启动位置 +0.5°", abs(target - 0.5) < 0.01, f"目标 {target:+.3f}°")
    engine.home_all()

    def render(width, height):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            panel.draw(width, height)
        return out.getvalue()

    def drawn_lines(frame):
        """把 draw 的输出切成 [(定位行号, 这一行的内容)]。末尾的收尾清屏不算一行。"""
        lines = []
        for part in frame.split(CLEAR_LINE):
            if part.startswith("\x1b["):
                head, _, content = part.partition("H")
                if content.startswith("\x1b[J"):
                    continue
                lines.append((int(head[2:].split(";")[0]), content))
        return lines

    frame = render(100, 30)
    drawn = drawn_lines(frame)
    check("绘制里没有换行符（不可能顶屏滚动）", "\n" not in frame)
    check("每行都用绝对定位，且行号连着",
          [row for row, _ in drawn] == list(range(1, len(drawn) + 1)),
          str([row for row, _ in drawn][:6]))
    check("画的行数不超过终端高度", len(drawn) <= 30, f"{len(drawn)} 行")
    check("轴表在（相对/圈内两栏都在）",
          "轴1" in frame and "相对" in frame and "圈内" in frame)
    narrow = render(40, 30)
    check("窄终端（40 列）也不超宽",
          all(visible_width(line) <= 40 for _, line in drawn_lines(narrow)))
    short = render(100, 8)
    check("矮终端（8 行）轴表仍在",
          "轴1" in short and "相对" in short, f"{len(drawn_lines(short))} 行")
    tiny = render(10, 3)
    check("太小只给一句提示", "终端太小" in tiny and "轴1" not in tiny)

    # 进/出备用屏幕：真终端上才有 termios，这里换掉让它跑一遍，只看输出序列。
    real_termios, real_tty = termios, tty
    globals()["termios"] = type("FakeTermios", (), {
        "TCSADRAIN": 0, "error": Exception,
        "tcgetattr": staticmethod(lambda fd: "saved"),
        "tcsetattr": staticmethod(lambda fd, when, attrs: None)})()
    globals()["tty"] = type("FakeTty", (), {"setraw": staticmethod(lambda fd: None)})()
    try:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            panel.enter_terminal()
            panel.leave_terminal()
        sequence = out.getvalue()
    finally:
        globals()["termios"], globals()["tty"] = real_termios, real_tty
    check("进出备用屏幕、收放光标的序列都在",
          all(token in sequence for token in
              (ALT_SCREEN_ON, ALT_SCREEN_OFF, HIDE_CURSOR, SHOW_CURSOR)))

    # 7) 往返验证（振幅 1°，跑够两趟）
    print("\n7) 往返验证")
    engine.home_all()
    drive(1.5)
    soak = SoakRun(engine, 0, 1.0)
    if not soak.start(time.monotonic()):
        check("往返验证能起来", False, engine.message)
    else:
        deadline = time.monotonic() + 30.0
        lowest = highest = engine._axis_position(0)
        while time.monotonic() < deadline and soak.laps < 2:
            now = time.monotonic()
            soak.step(now)
            engine.tick(now)
            position = engine._axis_position(0)
            lowest, highest = min(lowest, position), max(highest, position)
            time.sleep(1.0 / CONTROL_RATE_HZ)
        engine.poll_status()
        print("  " + soak.summary(time.monotonic()))
        check("跑够 2 趟", soak.laps >= 2, f"{soak.laps} 趟")
        span = engine.counts_to_deg(0, highest - lowest)
        check("确实来回动过", span > 0.5, f"行程 {span:.2f}°")
        check("没记到异常", not soak.anomalies, str(soak.anomalies[:2]))
        print("  " + soak.stop())
        engine.home_all()

    print("\n结束位置：" + engine.report_line())
    if failures:
        print("\n失败项：" + "，".join(failures))
        return 1
    print("\n全部通过")
    return 0


# ---------------------------------------------------------------- 入口


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="EtherCAT 主站手动控制台（全屏面板）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="与主站解耦：只走命令套接字，主站一行不改。")
    parser.add_argument("--deployment",
                        default=os.environ.get("DEPLOYMENT", "orangepi-bench-quint-30deg"),
                        help="部署 ID，套接字路径由它推导（默认 %(default)s）")
    parser.add_argument("--socket", default=None, help="直接指定套接字路径，覆盖 --deployment")
    parser.add_argument("--repo",
                        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        help="仓库根目录（默认按本文件位置推导）")
    parser.add_argument("--jog-speed", type=float, default=DEFAULT_JOG_SPEED,
                        help="点动速度，输出轴度/秒（默认 %(default)s）")
    parser.add_argument("--range", type=float, default=DEFAULT_RANGE_DEG,
                        help="自助软范围，相对启动位置多少度；0 表示不限制（默认 %(default)s）")
    parser.add_argument("--start", action="store_true", help="套接字不在时替你把主站拉起来")
    parser.add_argument("--allow-stop-master", action="store_true",
                        help="允许面板的 m 键停掉正在跑的主站")
    parser.add_argument("--selftest", action="store_true", help="不开面板，跑一遍引擎自检")
    parser.add_argument("--travel", type=float, default=1.0,
                        help="自检的位移，度（默认 %(default)s）")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    sock_path = args.socket or f"/tmp/emaster-{args.deployment}.sock"

    if args.selftest:
        return selftest(args)

    master = MasterProcess(args.repo, args.deployment, sock_path)
    if not master.socket_exists():
        if not args.start:
            print(f"套接字不存在：{sock_path}")
            print("先起主站，或者加 --start 让面板替你拉起来。")
            return 1
        problem = master.start()
        if problem:
            print("错误：" + problem)
            return 1
        print("已拉起主站，等它建出套接字…")
        for _ in range(120):
            if master.socket_exists():
                break
            time.sleep(0.5)
        else:
            print("主站没建出套接字。日志尾巴：")
            print(master.tail_log())
            return 1

    if termios is None:
        print("面板只能在 Unix 上跑（缺 termios）。本机试试 --selftest。")
        return 1
    if not sys.stdin.isatty():
        print("标准输入不是终端，全屏面板起不来。非交互环境请用 --selftest。")
        return 1

    client = ConsoleClient(sock_path)
    engine = Engine(client, args.deployment, args.jog_speed, args.range,
                    repo_root=args.repo)
    panel = Panel(engine, master, args)

    engine.attach()   # 失败也照进面板：面板上会说明原因，还能按 m 拉起主站
    try:
        panel.run()
    except KeyboardInterrupt:
        pass
    finally:
        client.disconnect()

    print("\n面板已退出。")
    pid = master.running_pid()
    if pid is not None:
        print(f"主站还在跑（pid {pid}），没动它。要停：kill -INT {pid}")
    report = master.report_path()
    if report:
        print(f"报告：{report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
