#!/usr/bin/env python3
"""
EtherCAT 主站交互式位置控制工具

用法：
    python3 interactive_control.py [socket_path]

设计要点（都是踩过的坑）：

1. 自动重连。命令服务器有 5 秒空闲超时（command_server.c 的
   COMMAND_CLIENT_IDLE_TIMEOUT_S），它有意为之——防止客户端进程被 kill 后
   留下半开连接占死 accept 队列。代价是客户端只要思考超过 5 秒，连接就被
   服务端单方面关闭，下一次 send 拿到 EPIPE。本工具在每次发送前检查连接
   闲置时长，并在失败时自动重连重试。

2. 大角度必须斜坡。主站对相邻两条目标做单步限幅（session_target.c），
   超限不是返回错误，而是判定 MOTION_INVALID 后经 note_runtime_failure
   中止整个会话。所以一步到位地发 30° 会直接打掉主站。本工具把目标拆成
   小步以固定频率流式发送，步长同时受 max_step 和设定速度约束。

3. max_step 从 topology 响应读取（主站新增字段），不硬编码——换部署时
   限幅值会变。

默认 socket 路径：/tmp/emaster-orangepi-bench-dual.sock
"""

import math
import re
import socket
import sys
import time

# 发送频率。需远高于主站的 200ms 外部目标看门狗（main.c）。
DEFAULT_RATE_HZ = 50.0

# 默认斜坡速度（输出轴度/秒）。取保守值：先能安全动起来，再按需调高。
DEFAULT_SPEED_DEG_PER_S = 20.0

# 单步不超过 max_step 的这个比例。留余量，避免因取整刚好越界。
MAX_STEP_MARGIN = 0.8

# 连接闲置超过这个秒数就主动重连，避开服务端的 5 秒超时。
IDLE_RECONNECT_S = 4.0


class EtherCATClient:
    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.buffer = b""
        self.last_io = 0.0

        self.state = None
        self.topology = None
        self.max_step = 0
        self.axis_count = 0

        # 最后一次成功提交的目标。斜坡的起点必须用它而不是实际位置：
        # 主站的单步限幅是拿新目标和上一条已提交目标比的。
        self.last_commanded = None

    # ---------- 连接管理 ----------

    def connect(self):
        self.disconnect()
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(5.0)
            self.sock.connect(self.sock_path)
            self.buffer = b""
            self.last_io = time.time()
            return True
        except OSError as exc:
            print(f"连接失败: {exc}")
            self.sock = None
            return False

    def disconnect(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.buffer = b""

    def _ensure_fresh(self):
        """闲置过久就重连，避免撞上服务端的 5 秒超时。"""
        if self.sock is None:
            return self.connect()
        if time.time() - self.last_io > IDLE_RECONNECT_S:
            return self.connect()
        return True

    def _read_response(self):
        """读到换行为止。服务端每条响应以 \\n 结尾。"""
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionResetError("服务端关闭了连接")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line.decode("utf-8", "replace").strip()

    def command(self, cmd, retries=2):
        """发送一条命令并返回响应；连接失效时自动重连重试。"""
        for attempt in range(retries + 1):
            if not self._ensure_fresh():
                if attempt == retries:
                    return None
                continue
            try:
                self.sock.sendall((cmd + "\n").encode("utf-8"))
                response = self._read_response()
                self.last_io = time.time()
                return response
            except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError) as exc:
                self.disconnect()
                if attempt == retries:
                    print(f"通信失败（已重试 {retries} 次）: {exc}")
                    return None
        return None

    # ---------- 查询 ----------

    def refresh_topology(self):
        response = self.command("topology")
        if not response or not response.startswith("OK|"):
            return False

        payload = response[3:]
        parts = payload.split("|")

        header = {}
        for item in parts[0].split(","):
            if "=" in item:
                key, value = item.split("=", 1)
                header[key] = value

        self.axis_count = int(header.get("axes", "0"))
        self.max_step = int(header.get("max_step", "0"))

        axes = []
        for part in parts[1:]:
            if not part.startswith("a"):
                continue
            axis = {}
            name, _, fields = part.partition(":")
            axis["id"] = name
            for item in fields.split(","):
                if "=" in item:
                    key, value = item.split("=", 1)
                    axis[key] = value
            axes.append(axis)
        self.topology = axes
        return True

    def refresh_status(self):
        response = self.command("status")
        if not response or not response.startswith("OK|"):
            return None

        payload = response[3:]
        parts = payload.split("|")

        status = {}
        for item in parts[0].split():
            if "=" in item:
                key, value = item.split("=", 1)
                status[key] = value

        axes = []
        for part in parts[1:]:
            if not part.startswith("a"):
                continue
            axis = {}
            name, _, fields = part.partition(":")
            axis["id"] = name
            for item in fields.split(","):
                if "=" in item:
                    key, value = item.split("=", 1)
                    axis[key] = value
            axes.append(axis)
        status["axes"] = axes

        self.state = int(status.get("state", "-1"))
        return status

    def wait_for_ready(self, timeout_s=10.0):
        """轮询直到主站进入 RUNNING（state=4）。"""
        print("等待主站就绪 (state=4)...")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            status = self.refresh_status()
            if status is None:
                return False
            if self.state == 4:
                print("主站已就绪")
                return True
            time.sleep(0.1)
        print(f"超时：主站未进入 RUNNING（当前 state={self.state}）")
        return False

    # ---------- 单位换算 ----------

    def counts_per_degree(self):
        """取第一轴的换算系数。主站的限幅是全轴统一值，斜坡步长同理。"""
        if not self.topology:
            return None
        axis = self.topology[0]
        try:
            increments = float(axis["enc"])
            gear_motor, gear_shaft = (float(x) for x in axis["gear"].split("/"))
        except (KeyError, ValueError):
            return None
        if gear_shaft == 0:
            return None
        return increments * (gear_motor / gear_shaft) / 360.0

    def degrees_to_counts(self, degrees):
        factor = self.counts_per_degree()
        if factor is None:
            return None
        return int(round(degrees * factor))

    def counts_to_degrees(self, counts):
        factor = self.counts_per_degree()
        if factor is None:
            return None
        return counts / factor

    # ---------- 运动 ----------

    def step_counts_for(self, speed_deg_per_s, rate_hz):
        """单步允许的最大 counts 增量：速度与单步限幅取小。"""
        factor = self.counts_per_degree()
        if factor is None:
            return None

        by_speed = speed_deg_per_s * factor / rate_hz
        if self.max_step > 0:
            by_step = self.max_step * MAX_STEP_MARGIN
            if by_speed > by_step:
                by_speed = by_step
        return max(1.0, by_speed)

    def move_to(self, targets, speed_deg_per_s, rate_hz):
        """
        把各轴从最后一条已提交目标斜升到 targets。

        未提交过目标时（首次移动）以实际位置为起点。
        """
        if len(targets) != self.axis_count:
            print(f"错误：需要 {self.axis_count} 个目标，收到 {len(targets)} 个")
            return False

        start = self.last_commanded
        if start is None:
            status = self.refresh_status()
            if status is None or len(status["axes"]) != self.axis_count:
                print("错误：无法确定起始位置")
                return False
            start = [int(axis["pos"]) for axis in status["axes"]]

        step_limit = self.step_counts_for(speed_deg_per_s, rate_hz)
        if step_limit is None:
            print("错误：拓扑信息不完整，无法换算")
            return False

        deltas = [target - origin for target, origin in zip(targets, start)]
        largest = max(abs(delta) for delta in deltas)
        if largest == 0:
            print("目标与当前位置相同，无需移动")
            return True

        steps = max(1, int(math.ceil(largest / step_limit)))
        interval = 1.0 / rate_hz
        duration = steps * interval

        print(f"斜坡：{len(targets)} 轴，{steps} 步，步长上限 {step_limit:.0f} counts，"
              f"{rate_hz:.0f}Hz，约 {duration:.2f}s")
        print(f"      起点 {start}")
        print(f"      终点 {targets}")

        for index in range(1, steps + 1):
            fraction = index / steps
            positions = [int(round(origin + delta * fraction))
                         for origin, delta in zip(start, deltas)]
            response = self.command(
                "set_external_target " + " ".join(str(p) for p in positions))
            if response is None or not response.startswith("OK"):
                print(f"第 {index}/{steps} 步失败: {response}")
                print("已中止；主站保持最后一条成功提交的目标")
                return False
            self.last_commanded = positions
            time.sleep(interval)

        print("斜坡完成")
        return True


def print_status(status, client):
    """格式化打印状态。"""
    factor = client.counts_per_degree()
    print("\n" + "=" * 80)
    print(f"主站状态: state={status.get('state')} cycle={status.get('cycle')} "
          f"enabled={status.get('enabled')} completed={status.get('completed')}")
    print("-" * 80)

    for axis in status["axes"]:
        pos = int(axis["pos"])
        target = int(axis.get("target_pos", pos))
        line = (f"{axis['id']}: pos={pos:9d} target={target:9d} "
                f"vel={int(axis['vel']):5d} torque={int(axis['torque']):5d} "
                f"status={axis['status']} err={axis['err']} state={axis['state']}")
        if factor:
            line += f"\n      pos={pos / factor:8.2f}°  target={target / factor:8.2f}°"
        print(line)

    if client.last_commanded is not None:
        print("-" * 80)
        print(f"最后提交目标: {client.last_commanded}")
    print("=" * 80)


HELP = """
可用命令:
  s, status              查看当前状态（位置、速度、力矩、状态字）
  t, topology            查看拓扑（编码器、减速比、单步限幅）
  g <c1> <c2> ...        绝对移动，单位 counts
  d <d1> <d2> ...        绝对移动，单位度（输出轴）
  r <d1> <d2> ...        相对移动，单位度（相对最后一条已提交目标）
  speed <deg/s>          设置斜坡速度，默认 {default_speed}
  rate <hz>              设置发送频率，默认 {default_rate}
  h, help                显示本帮助
  q, quit                退出

说明:
  大角度移动会自动拆成小步流式发送。主站对相邻两条目标的增量有硬限幅，
  单步超限会中止整个会话（不是拒绝这一条命令），所以不能一步到位地发大目标。

  Ctrl+C 可中断正在进行的斜坡，主站保持最后一条成功提交的目标。
""".format(default_speed=DEFAULT_SPEED_DEG_PER_S, default_rate=DEFAULT_RATE_HZ)


def interactive_shell(client):
    print("\n" + "=" * 80)
    print("交互式控制模式")
    print("=" * 80)
    print(HELP)

    speed = DEFAULT_SPEED_DEG_PER_S
    rate = DEFAULT_RATE_HZ

    while True:
        try:
            raw = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not raw:
            continue

        parts = raw.split()
        action = parts[0].lower()

        if action in ("q", "quit"):
            break

        if action in ("h", "help"):
            print(HELP)
            continue

        if action in ("s", "status"):
            status = client.refresh_status()
            if status:
                print_status(status, client)
            else:
                print("获取状态失败（主站可能已停止）")
            continue

        if action in ("t", "topology"):
            if client.refresh_topology():
                print(f"\n轴数={client.axis_count} 单步限幅={client.max_step} counts")
                for axis in client.topology:
                    print(f"  {axis['id']}: bus={axis['bus']} enc={axis['enc']} "
                          f"gear={axis['gear']}")
                factor = client.counts_per_degree()
                if factor:
                    print(f"  换算: 1° = {factor:.2f} counts，"
                          f"单步限幅 ≈ {client.max_step / factor:.2f}°")
                print()
            else:
                print("获取拓扑失败")
            continue

        if action == "speed":
            if len(parts) != 2:
                print("用法: speed <deg/s>")
                continue
            try:
                value = float(parts[1])
            except ValueError:
                print("错误：速度必须是数字")
                continue
            if value <= 0:
                print("错误：速度必须为正")
                continue
            speed = value
            print(f"斜坡速度 = {speed} °/s")
            continue

        if action == "rate":
            if len(parts) != 2:
                print("用法: rate <hz>")
                continue
            try:
                value = float(parts[1])
            except ValueError:
                print("错误：频率必须是数字")
                continue
            if value <= 0.5:
                print("错误：频率过低，主站的 200ms 看门狗会先切到 HOLD")
                continue
            rate = value
            print(f"发送频率 = {rate} Hz")
            continue

        if action in ("g", "d", "r"):
            if len(parts) < 2:
                print(f"用法: {action} <值1> <值2> ...")
                continue

            base = client.last_commanded
            if action == "r" and base is None:
                status = client.refresh_status()
                if status is None:
                    print("错误：无法确定当前位置")
                    continue
                base = [int(axis["pos"]) for axis in status["axes"]]

            try:
                values = [float(v) for v in parts[1:]]
            except ValueError:
                print("错误：参数必须是数字")
                continue

            if len(values) != client.axis_count:
                print(f"错误：需要 {client.axis_count} 个参数，收到 {len(values)} 个")
                continue

            if action == "g":
                targets = [int(round(v)) for v in values]
            else:
                counts = [client.degrees_to_counts(v) for v in values]
                if any(c is None for c in counts):
                    print("错误：拓扑信息不完整，无法换算")
                    continue
                targets = ([base[i] + counts[i] for i in range(len(counts))]
                           if action == "r" else counts)

            try:
                client.move_to(targets, speed, rate)
            except KeyboardInterrupt:
                print("\n斜坡被中断；主站保持最后一条成功提交的目标")
            continue

        print(f"未知命令: {parts[0]}，输入 'help' 查看帮助")

    return 0


def main():
    sock_path = (sys.argv[1] if len(sys.argv) > 1
                 else "/tmp/emaster-orangepi-bench-dual.sock")

    print("EtherCAT 主站交互式位置控制工具")
    print("=" * 80)
    print(f"Socket 路径: {sock_path}")

    client = EtherCATClient(sock_path)

    if not client.connect():
        return 1
    if not client.wait_for_ready():
        client.disconnect()
        return 1
    if not client.refresh_topology():
        print("警告：无法获取拓扑，角度换算不可用（只能用 g 命令按 counts 操作）")
    else:
        factor = client.counts_per_degree()
        print(f"轴数={client.axis_count} 单步限幅={client.max_step} counts"
              + (f" ≈ {client.max_step / factor:.2f}°" if factor else ""))

    status = client.refresh_status()
    if status:
        print_status(status, client)

    try:
        return interactive_shell(client)
    finally:
        client.disconnect()
        print("\n已断开连接")


if __name__ == "__main__":
    sys.exit(main())
