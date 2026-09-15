#!/usr/bin/env python3
"""按最终场景的入口与流向驱动轴：Unix 域套接字 -> set_external_target。

流向与最终部署一致：客户端 -> 命令套接字 -> 主站外部目标缓冲区 ->
周期回调 position_target_source -> CSP 目标。不启用部署里的内部运动曲线，
也不绕过命令协议直接改目标。格式（协议 v1 文本）是当前的临时形态，入口和
流向不变。

两种运动模式：
  单程（默认）      起点 -> 起点+位移，之后在 --hold 秒内持续重发同一目标
  往返（--cycles）  起点 <-> 起点+位移，连续往返；--cycles 0 表示不限次数

用法:
  python3 test_external_motion_client.py <socket_path> [选项]

选项:
  --degrees D    各轴相对位移（输出轴度数，默认 30）
  --duration S   单程模式的位移耗时（秒，默认 3）
  --rate HZ      目标更新频率（默认 50，必须快于 200ms 超时）
  --hold S       单程模式到位后保持发送的秒数（默认 60）
  --ready S      等待 RUNNING 的超时（秒，默认 30）
  --cycles N     往返次数（0 = 不限次数，直到 --total 或主站结束）
  --traverse S   往返模式单程耗时（秒，默认 3）
  --wave W       往返波形：triangle（默认，匀速）或 sine（两端速度为零）
  --total S      往返模式总时长上限（秒，0 = 不限）
  --dry-run      不连主站，只按参数生成目标、核对限幅并打印统计

退出时停止发送，主站自动进入 HOLD（保持最后目标），不停机。

"持续有效"的判据：主站在 200ms 收不到新目标就判定客户端失活、转 HOLD。
本脚本以墙钟为时间基准生成曲线，并记录相邻两条目标的实际发送间隔；结束时
报告最大间隔，超过 200ms 即说明中途出现过不受控的保持窗口。
"""

import argparse
import math
import socket
import sys
import time

IDLE_TIMEOUT_S = 5.0
TARGET_TIMEOUT_MS = 200
# 往返模式的单步限幅裕量：实际可达更新频率低于标称值（每次命令一个往返），
# 曲线按墙钟推进，频率越低单步越大。按标称值的这个比例估算，留出余量。
RATE_MARGIN = 0.6


class Client:
    def __init__(self, socket_path, ready_timeout):
        self.socket_path = socket_path
        self.ready_timeout = ready_timeout
        self.sock = None
        self.stream = None

    def connect(self):
        self.close()
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(self.socket_path)
                self.stream = self.sock.makefile("rwb")
                return
            except OSError as error:
                self.close()
                if time.monotonic() > deadline:
                    raise SystemExit(f"连接失败: {error}")
                time.sleep(0.2)

    def close(self):
        if self.stream is not None:
            try:
                self.stream.close()
            except OSError:
                pass
            self.stream = None
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def command(self, text):
        """发送一条命令并读回响应。连接失效时抛 OSError。"""
        if self.stream is None:
            self.connect()
        try:
            self.stream.write((text + "\n").encode("utf-8"))
            self.stream.flush()
            line = self.stream.readline()
        except OSError:
            self.close()
            raise
        if not line:
            self.close()
            raise ConnectionError("服务端关闭了连接")
        return line.decode("utf-8", errors="replace").strip()

    def wait_running(self):
        deadline = time.monotonic() + self.ready_timeout
        while True:
            response = self.command("status")
            state = parse_field(response, "state")
            if state == 4:
                return response
            if time.monotonic() > deadline:
                raise SystemExit(f"等待 RUNNING 超时，最后状态: {response}")
            time.sleep(0.1)


def parse_field(response, key):
    """从 OK|k=v k=v 形式的响应里取整数值。

    分隔符有两种：段内用逗号，段之间用竖线。只切逗号会让最后一个字段吞掉
    后面的 "|a1:..."，例如 topology 的 max_step 会被解析成 "2548|a1:bus=1"
    而取不到值——单步限幅因此在联机时被静默跳过。
    """
    if not response.startswith("OK|"):
        return None
    for token in response[3:].replace(",", " ").replace("|", " ").split():
        if token.startswith(key + "="):
            value = token[len(key) + 1:]
            try:
                return int(value, 0)
            except ValueError:
                return None
    return None


def parse_axis_positions(response):
    """取每轴实际位置，返回按总线位置排序的列表。"""
    positions = []
    for segment in response.split("|"):
        if not segment.startswith("a"):
            continue
        fields = dict(
            item.split("=", 1) for item in segment.split(":", 1)[1].split(",") if "=" in item
        )
        if "pos" in fields:
            positions.append(int(fields["pos"], 0))
    return positions


def counts_per_degree(topology):
    """按拓扑返回的编码器分辨率和减速比换算 counts/度（输出轴）。

    减速比字段是 "电机转数/输出轴转数"（如 gear=28/1），大角度按输出轴度量。
    """
    enc = parse_field(topology, "enc") or 16384
    gear = 28.0
    if topology.startswith("OK|"):
        for token in topology.replace(",", " ").split():
            if token.startswith("gear=") and "/" in token:
                numerator, _, denominator = token[len("gear="):].partition("/")
                try:
                    if float(denominator) != 0.0:
                        gear = float(numerator) / float(denominator)
                except ValueError:
                    pass
                break
    return enc * gear / 360.0


def triangle_phase(t, traverse):
    """三角波：t=0 起 0 -> 1 -> 0，周期 2*traverse，全程匀速。"""
    period = 2.0 * traverse
    x = math.fmod(t, period)
    if x < 0.0:
        x += period
    return x / traverse if x <= traverse else 2.0 - x / traverse


def sine_phase(t, traverse):
    """正弦波：t=0 起 0 -> 1 -> 0，周期 2*traverse，两端速度为零。"""
    return 0.5 * (1.0 - math.cos(math.pi * t / traverse))


def check_step_limit(delta, traverse, rate, max_step):
    """往返模式下按标称频率核对单步增量，必要时拉长单程时间。

    返回 (traverse, message)。max_step 为 0 表示主站未启用单步限幅。
    """
    if max_step <= 0:
        return traverse, None
    span = max(abs(value) for value in delta)
    if span == 0:
        return traverse, None
    per_update = span / (traverse * rate * RATE_MARGIN)
    if per_update <= max_step:
        return traverse, None
    required = span / (max_step * rate * RATE_MARGIN)
    return required, (f"[client] 单步 {per_update:.0f} counts 超过 max_step={max_step}，"
                      f"单程时间 {traverse:.2f}s 拉长到 {required:.2f}s "
                      f"（每步约 {span / (required * rate * RATE_MARGIN):.0f} counts）")


def dry_run(start, delta, args):
    """不连主站，按参数生成一个往返周期的目标序列并核对步长。"""
    phase_fn = sine_phase if args.wave == "sine" else triangle_phase
    traverse, message = check_step_limit(delta, args.traverse, args.rate, args.max_step)
    if message:
        print(message, flush=True)
    period = 1.0 / args.rate
    steps = max(1, int(traverse * args.rate * 2.0))
    prev = list(start)
    max_step = 0
    samples = []
    for index in range(steps + 1):
        t = index * period
        f = phase_fn(t, traverse)
        frame = [int(round(p + d * f)) for p, d in zip(start, delta)]
        step = max(abs(a - b) for a, b in zip(frame, prev))
        max_step = max(max_step, step)
        prev = frame
        if index % max(1, steps // 8) == 0 or index == steps:
            samples.append((t, f, frame))
    print(f"[dry-run] 波形={args.wave} 单程={traverse:.2f}s 频率={args.rate:.0f}Hz "
          f"样本数={steps + 1}")
    for t, f, frame in samples:
        print(f"[dry-run] t={t:7.3f}s phase={f:.4f} 目标={frame}")
    print(f"[dry-run] 单步最大 {max_step} counts "
          f"（max_step={args.max_step}，{'未启用限幅' if args.max_step <= 0 else '限内' if max_step <= args.max_step else '超限'})")
    return max_step


def run_reciprocate(client, start, delta, args, max_step):
    """连续往返：起点 <-> 起点+位移，直到次数用尽、总时长到点或连接中断。"""
    phase_fn = sine_phase if args.wave == "sine" else triangle_phase
    period = 1.0 / args.rate
    base = time.monotonic()
    t_next = base
    last_send = None
    max_gap = 0.0
    max_step_sent = 0
    sent = 0
    rejected = 0
    prev_frame = list(start)
    next_report = base + 10.0
    stop_reason = "循环结束"
    elapsed = 0.0

    try:
        while True:
            elapsed = time.monotonic() - base
            if args.cycles > 0 and elapsed >= args.cycles * 2.0 * args.traverse:
                stop_reason = f"完成 {args.cycles} 次往返"
                break
            if args.total > 0 and elapsed >= args.total:
                stop_reason = f"到达总时长 {args.total:.0f}s"
                break

            f = phase_fn(elapsed, args.traverse)
            frame = [int(round(p + d * f)) for p, d in zip(start, delta)]
            response = client.command("set_external_target " + " ".join(str(v) for v in frame))
            sent += 1
            now = time.monotonic()
            if last_send is not None and now - last_send > max_gap:
                max_gap = now - last_send
            last_send = now
            step = max(abs(a - b) for a, b in zip(frame, prev_frame))
            max_step_sent = max(max_step_sent, step)
            prev_frame = frame

            if not response.startswith("OK|"):
                rejected += 1
                print(f"[client] 第 {sent} 条目标被拒: {response}", flush=True)
                if "Not ready" in response:
                    client.wait_running()

            if now >= next_report:
                feedback = client.command("status")
                positions = parse_axis_positions(feedback) if feedback.startswith("OK|") else []
                print(f"[client] t+{elapsed:.0f}s 往返 {elapsed / (2.0 * args.traverse):.2f} 次 "
                      f"phase={f:.3f} 目标={frame} 实际={positions} "
                      f"已发={sent} 被拒={rejected} 最大间隔={max_gap * 1000:.1f}ms", flush=True)
                next_report = now + 10.0

            t_next += period
            now = time.monotonic()
            if t_next < now:
                # 单次往返（命令+响应）超过周期时不追赶：追赶会连发多条，
                # 目标间隔反而变短，曲线时间基准也会偏离墙钟。
                t_next = now
            time.sleep(t_next - now)
    except (OSError, ConnectionError) as error:
        stop_reason = "连接中断"
        print(f"[client] 发送中断: {error}", flush=True)

    client.close()
    print(f"[client] 结束（{stop_reason}）：墙钟 {elapsed:.1f}s，"
          f"完成往返 {elapsed / (2.0 * args.traverse):.2f} 次，"
          f"发出 {sent} 条（{sent / max(elapsed, 1e-9):.1f} 条/秒），被拒 {rejected} 条", flush=True)
    print(f"[client] 相邻目标间隔 最大 {max_gap * 1000:.1f} ms "
          f"（主站失活阈值 {TARGET_TIMEOUT_MS} ms）", flush=True)
    print(f"[client] 单步最大 {max_step_sent} counts（上限 {max_step}）", flush=True)
    if stop_reason == "连接中断":
        # 主站侧结束（故障锁存或停机）：命令流的终点由主站的报告给出，客户端只报事实。
        print("[client] 命令流被主站中断，未跑满计划时长", flush=True)
        return 2
    if max_gap * 1000.0 >= TARGET_TIMEOUT_MS:
        print("[client] 警告：间隔超过 200ms，主站中途转为 HOLD，命令流不是全程连续有效的",
              flush=True)
        return 1
    print("[client] 命令流全程连续有效（未出现超过 200ms 的空档）", flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket_path", nargs="?", default=None)
    parser.add_argument("--degrees", type=float, default=30.0)
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--rate", type=float, default=50.0)
    parser.add_argument("--hold", type=float, default=60.0)
    parser.add_argument("--ready", type=float, default=30.0)
    parser.add_argument("--cycles", type=int, default=None,
                        help="往返次数；0 = 不限次数（不给则用单程模式）")
    parser.add_argument("--traverse", type=float, default=3.0, help="往返单程耗时（秒）")
    parser.add_argument("--wave", choices=("triangle", "sine"), default="triangle")
    parser.add_argument("--total", type=float, default=0.0, help="往返总时长上限（秒）")
    parser.add_argument("--max-step", type=int, default=2548,
                        help="单步上限（counts），仅 --dry-run 使用；"
                             "联机时以 topology 返回的 max_step 为准（默认 2548 = 2°@16384×28:1）")
    parser.add_argument("--dry-run", action="store_true",
                        help="不连主站，只生成目标并核对限幅")
    args = parser.parse_args()

    if args.rate <= 0 or 1.0 / args.rate >= TARGET_TIMEOUT_MS / 1000.0:
        raise SystemExit("更新频率必须快于外部目标超时（200ms）")

    if args.dry_run:
        # 本地核对用：按台架的编码器折算（16384 counts/rev × 28:1 减速比），
        # 起点取 0，不连主站、不碰硬件。
        per_degree = counts_per_degree("OK|axes=1,enc=16384|a1:bus=1,enc=16384,gear=28/1")
        delta = [int(round(args.degrees * per_degree))] * 3
        print(f"[dry-run] counts/度={per_degree:.2f} 位移={delta[0]} counts", flush=True)
        max_step = dry_run([0, 0, 0], delta, args)
        return 0 if max_step <= args.max_step else 1

    if args.socket_path is None:
        raise SystemExit("缺少套接字路径（--dry-run 可离线核对波形）")

    client = Client(args.socket_path, args.ready)
    client.connect()

    status = client.wait_running()
    topology = client.command("topology")
    start = parse_axis_positions(status)
    if not start:
        raise SystemExit(f"无法从 status 解析轴位置: {status}")

    per_degree = counts_per_degree(topology)
    delta = int(round(args.degrees * per_degree))
    max_step = parse_field(topology, "max_step") or 0
    print(f"[client] counts/度={per_degree:.2f} 位移={delta} counts max_step={max_step}",
          flush=True)
    print(f"[client] 起点={start}", flush=True)

    if args.cycles is not None:
        traverse, message = check_step_limit([delta] * len(start), args.traverse,
                                             args.rate, max_step)
        if message:
            print(message, flush=True)
        args.traverse = traverse
        print(f"[client] 往返模式：{start} <-> "
              f"{[position + delta for position in start]}，单程 {traverse:.2f}s，"
              f"波形 {args.wave}，"
              f"{'不限次数' if args.cycles == 0 else f'{args.cycles} 次'}",
              flush=True)
        return run_reciprocate(client, start, [delta] * len(start), args, max_step)

    targets = [position + delta for position in start]
    print(f"[client] 终点={targets}", flush=True)

    period = 1.0 / args.rate
    steps = max(1, int(args.duration * args.rate))
    # 单步限幅：主站在周期线程里按相邻两条目标之差判定，超限会判 MOTION_INVALID
    # 并中止整个会话（协议 7.2 节）。步长按每次更新的增量折算，超过 max_step 时
    # 拉长位移时间而不是拒绝。
    if max_step > 0 and abs(delta) > 0:
        per_update = (abs(delta) + steps - 1) // steps
        if per_update > max_step:
            steps = (abs(delta) + max_step - 1) // max_step
            period = max(period, args.duration / steps) if steps else period
            print(f"[client] 单步 {per_update} counts 超过 max_step={max_step}，"
                  f"改为 {steps} 步（每步约 {abs(delta) // steps} counts）", flush=True)
    sent = 0
    errors = 0
    try:
        for step in range(1, steps + 1):
            frame = [position + delta * step // steps for position in start]
            response = client.command("set_external_target " + " ".join(str(v) for v in frame))
            sent += 1
            if not response.startswith("OK|"):
                errors += 1
                print(f"[client] 第 {step} 步被拒: {response}", flush=True)
                if "Not ready" in response:
                    client.wait_running()
            if step % max(1, steps // 10) == 0:
                print(f"[client] 位移 {step}/{steps} 目标={frame}", flush=True)
            time.sleep(period)

        print(f"[client] 到位，保持 {args.hold}s（每 {period:.3f}s 发送一次，避免 200ms 超时转 HOLD）",
              flush=True)
        hold_until = time.monotonic() + args.hold
        feedback = client.command("status")
        if feedback.startswith("OK|"):
            print(f"[client] 保持中实际位置={parse_axis_positions(feedback)}", flush=True)
        next_report = time.monotonic() + 5.0
        while time.monotonic() < hold_until:
            client.command("set_external_target " + " ".join(str(v) for v in targets))
            sent += 1
            time.sleep(period)
            if time.monotonic() >= next_report:
                feedback = client.command("status")
                if feedback.startswith("OK|"):
                    print(f"[client] t+{args.hold - (hold_until - time.monotonic()):.0f}s "
                          f"实际位置={parse_axis_positions(feedback)}", flush=True)
                next_report += 5.0
    except (OSError, ConnectionError) as error:
        print(f"[client] 发送中断: {error}", flush=True)

    client.close()
    print(f"[client] 结束：发出 {sent} 条目标，被拒 {errors} 条", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
