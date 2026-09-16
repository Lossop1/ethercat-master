#!/usr/bin/env python3
"""闭环客户端：外部策略侧真实用法的可执行样板。

存在的理由。此前台架上量过的所有东西——帧距、截止时间、WKC、背压——回答的都是
"主站自己健不健康"。那是必要条件，不是最终用法。最终用法是一条回路：

    主站采样位置 -> 观测通道 -> 策略取回 -> 算目标 -> 命令通道 -> 驱动器执行

回路上的延迟决定策略能开多高的增益，回路上的完整性决定策略能不能信任手里的数据。
这个客户端就按那条回路跑，并把"策略真正感受到的量"记下来：

1. **观测数据年龄**：帧自带的 ``mono_ns`` 是主站在发布点取的 CLOCK_MONOTONIC，与
   客户端同源同时基，两者相减就是"这一拍的位置从被采到、到出现在我手里"。策略拿
   它算动作，所以它直接进回路的相位裕度。

   这个数**与客户端的读取频率无关**：``LATEST`` 取的是最新一帧，而主站每周期都发一
   帧，所以无论什么时候去看，手里那一帧最多只旧一个周期（1 ms），均值半个周期。
   这是"按客户端频率取最新"这个设计的直接好处，也是它与"客户端自己以 50 Hz 采
   集"的分野——后者会再叠半个策略周期（10 ms）的等待。

2. **命令生效延迟**：我发出目标，到它作为 ``target_pos`` 出现在某帧观测里。用的是
   那一帧自己的 ``mono_ns`` 而不是我收到的时刻，否则测到的是"我隔了多久才再看
   一眼"。分辨率为一个主站周期：目标在 1 ms 栅格上的哪一格生效，只能在下一格看
   出来。

3. **跟随误差** ``ref - actual``，以及**帧完整性**（cycle 增量、flags、WKC）。

两种模式：

``--mode readonly``
    只读，不发目标。量纯粹的取数延迟——没有控制命令干扰，可以跑到 1 kHz。

``--mode closed``
    读一帧、按余弦往返算参考轨迹、可选地叠加位置反馈、发回去。这是策略的实际工作
    方式，也是"闭环"的字面意思。

**相位锁定的陷阱**：若 ``--read-hz`` 与主站周期成整数比（1 kHz 的整除数，比如
50/200/500），客户端的采样相位会与主站的发布相位锁死，测出来的年龄分布会异常地
窄——那不是真实分布，是一次幸运的相位对齐。要看真实分布，用与周期不整除的频率，
或者给 ``--jitter-us`` 加抖动把相位扫开。

用法：
    python3 closed_loop_client.py --obs-socket /tmp/emaster-<id>-obs.sock \\
        --mode readonly --hz 1000 --jitter-us 1000 --seconds 30
    python3 closed_loop_client.py --obs-socket ... --cmd-socket /tmp/emaster-<id>.sock \\
        --mode closed --hz 50 --read-hz 1000 --seconds 60 --kp 0.3

退出码：0 正常收尾；2 拿不到观测；3 命令通道不可用（模式不对、被拒、被断开）。
"""

import argparse
import json
import math
import os
import random
import socket
import sys
import time

# 与 include/emaster/bus/command_socket_path.h 的 EMASTER_DEPLOYMENT_ENV 同名同义。
DEPLOYMENT_ENV = "EMASTER_DEPLOYMENT"

# 台架四轴的换算：16384 增量/电机转 × 28 减速比 ÷ 360 度。仅用于把 counts 折成度
# 打印出来给人看，判据一律用 counts，免得引入一个会漂的浮点常数。
COUNTS_PER_SHAFT_DEG = 16384.0 * 28.0 / 360.0
DEFAULT_AMPLITUDE_COUNTS = int(round(30.0 * COUNTS_PER_SHAFT_DEG))
# 心跳周期：长跑日志里要能区分"在跑"和"卡死"。
HEARTBEAT_NS = 60 * 10 ** 9


def percentile(ordered, q):
    """有序序列的分位数（线性插值）。序列必须已排序。"""
    if not ordered:
        return None
    if len(ordered) == 1:
        return float(ordered[0])
    position = q * (len(ordered) - 1)
    lower = int(math.floor(position))
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarise(values):
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    return {
        "n": len(ordered),
        "min": ordered[0],
        "p50": percentile(ordered, 0.50),
        "p90": percentile(ordered, 0.90),
        "p99": percentile(ordered, 0.99),
        "max": ordered[-1],
    }


def scaled(summary, divisor, digits):
    """把一份统计量整体换个单位。'n' 是计数，不参与换算。"""
    out = {}
    for key, value in summary.items():
        if key == "n":
            out[key] = value
        elif isinstance(value, (int, float)):
            out[key] = round(value / divisor, digits)
        else:
            out[key] = value
    return out


def connect(path, timeout):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(path)
    return sock


class LineReader:
    """按行读。观测与命令两个通道的响应都以换行结尾，一条响应就是一行。"""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = bytearray()

    def line(self):
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                raw = bytes(self.buffer[:newline])
                del self.buffer[:newline + 1]
                return raw.decode("utf-8", errors="replace")
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("连接在对端发出完整行之前关闭")
            self.buffer += chunk


def parse_latest(reply):
    """把 LATEST 的文本响应解成 (frame, axes)。

    形状（注意分隔符有两级）：::

        OK|publish_index=6121 cycle=7325 mono_ns=... axes=4 flags=0x0|a1:pos=..,vel=..
        ^  帧级字段之间是空格                      ^ 轴级字段之间是逗号

    任何不自洽都不猜：返回 (None, None) 让调用方重问，而不是按错位的字段算出一个
    看起来合理的关节角。这条通道喂的是控制策略。
    """
    parts = reply.split("|")
    if not parts or parts[0] != "OK":
        return None, None
    frame = {}
    axes = []
    for part in parts[1:]:
        if part.startswith("a") and ":" in part:
            _, _, body = part.partition(":")
            axis = {}
            for item in body.split(","):
                key, _, value = item.partition("=")
                try:
                    axis[key] = int(value, 0)
                except ValueError:
                    return None, None
            if "pos" not in axis or "target_pos" not in axis:
                return None, None
            axes.append(axis)
        elif "=" in part:
            for item in part.split():
                key, _, value = item.partition("=")
                if not key:
                    continue
                try:
                    frame[key] = int(value, 0)
                except ValueError:
                    frame[key] = value
    if "mono_ns" not in frame or "cycle" not in frame:
        return None, None
    if not axes:
        return None, None
    return frame, axes


_PARSE_FAILURE_REPORTED = False


def read_latest(obs, reader):
    """取最新一帧，返回 (frame, axes, received_ns, unstable)。

    received_ns 在整行收齐之后立刻取——晚一步取就会把解析开销算进延迟里。
    """
    global _PARSE_FAILURE_REPORTED
    obs.sendall(b"LATEST\n")
    reply = reader.line()
    received_ns = time.monotonic_ns()
    frame, axes = parse_latest(reply)
    if frame is None:
        # 解析失败曾经表现为"10 秒拿不到任何帧"，而原因藏在别处。第一次就把原始
        # 回包打出来，下次一眼能看出是协议变了还是主站还没发布。
        if not _PARSE_FAILURE_REPORTED:
            _PARSE_FAILURE_REPORTED = True
            print(f"LATEST 解不动，原始回包前 300 字节：{reply[:300]!r}", flush=True)
        return None, None, received_ns, True
    return frame, axes, received_ns, False


def deployment_sockets(deployment_id):
    """按主站的命名规则由部署 ID 推出命令与观测两个路径。

    格式串与 include/emaster/bus/command_socket_path.h 是同一套：命令
    /tmp/emaster-<id>.sock，观测 /tmp/emaster-<id>-obs.sock。两处各写一份是
    有意的——它们分属 C 与 Python 两个构建，共享不了那个头文件；但规则是同一
    条，改命名时两处一起改。
    """
    return (f"/tmp/emaster-{deployment_id}.sock",
            f"/tmp/emaster-{deployment_id}-obs.sock")


def resolve_sockets(args, parser):
    """填上缺省的 socket 路径。显式路径优先，其次 --deployment，其次环境变量。

    不去猜默认部署：此前 CLI 工具各自写死一个路径字面量，连错主站却看不出原因，
    这一点在 command_socket_path.h 的注释里已经写过。这里沿用同一条规矩——没有
    部署 ID 可用时直接报错退出。
    """
    if args.obs_socket is not None:
        return
    deployment = args.deployment or os.environ.get(DEPLOYMENT_ENV)
    if not deployment:
        parser.error("要么给 --obs-socket，要么给 --deployment（或设 EMASTER_DEPLOYMENT）")
    command_path, observation_path = deployment_sockets(deployment)
    args.obs_socket = observation_path
    if args.cmd_socket is None and args.mode == "closed":
        args.cmd_socket = command_path


def run(args):
    if args.mode == "closed" and not args.cmd_socket:
        print("closed 模式必须给 --cmd-socket（或用 --deployment 推导）", file=sys.stderr)
        return 3
    if args.mode == "readonly" and args.cmd_socket:
        print("readonly 模式不接受 --cmd-socket（它要量的就是没有命令的样子）",
              file=sys.stderr)
        return 3

    try:
        obs = connect(args.obs_socket, args.timeout)
    except OSError as exc:
        print(f"连不上观测通道 {args.obs_socket}：{exc}", file=sys.stderr)
        return 2
    reader = LineReader(obs)
    obs.sendall(b"INFO\n")
    print(f"观测通道 INFO：{reader.line()}", flush=True)

    cmd = None
    cmd_reader = None
    if args.mode == "closed":
        try:
            cmd = connect(args.cmd_socket, args.timeout)
        except OSError as exc:
            print(f"连不上命令通道 {args.cmd_socket}：{exc}", file=sys.stderr)
            return 3
        cmd_reader = LineReader(cmd)

    # 起点取自观测帧本身，不另外查 status：一来少一次往返，二来 status 与观测是两条
    # 通道、两个时刻，另取的值可能与真正开始运动时的那一刻对不上。
    start_positions = None
    deadline = time.monotonic() + 10.0
    while start_positions is None:
        if time.monotonic() > deadline:
            print("10 秒内没有拿到任何观测帧（EMASTER_OBSERVATION 开了吗？）", file=sys.stderr)
            return 2
        frame, axes, _, unstable = read_latest(obs, reader)
        if unstable:
            time.sleep(0.002)
            continue
        start_positions = [axis["pos"] for axis in axes]
    axis_count = len(start_positions)
    print(f"起点 counts={start_positions} "
          f"(≈{[round(v / COUNTS_PER_SHAFT_DEG, 2) for v in start_positions]} 度)", flush=True)

    period_ns = int(1e9 / args.read_hz)
    send_every = max(1, int(round(args.read_hz / args.hz))) if args.mode == "closed" else 0
    begin_ns = time.monotonic_ns()
    finish_ns = begin_ns + int(args.seconds * 1e9)

    obs_latency = []
    cmd_latency = []
    following_error = []
    loop_ticks = []
    unstable_reads = 0
    flagged_frames = 0
    observed_frames = 0
    wkc_histogram = {}
    cycle_delta_ok = 0
    cycle_delta_total = 0
    previous_cycle = None
    pending = []
    cmd_sent = 0
    cmd_matched = 0
    cmd_unmatched = 0
    cmd_errors = 0

    next_tick = begin_ns
    tick = 0
    heartbeat_next = begin_ns + HEARTBEAT_NS
    divergence_reports = 0
    # 逐轴而不是合并：某个驱动器退出使能时，它的跟随误差会独自冲高，
    # 合并成一个最大值就被另外三轴淹没了。
    axis_error_max = [0.0] * axis_count
    while True:
        now = time.monotonic_ns()
        if now >= finish_ns:
            break
        if now < next_tick:
            time.sleep((next_tick - now) / 1e9)
        elif now > next_tick + period_ns:
            # 落后超过一整拍：重新对齐，而不是补发。补发会造出一族假的短间隔，
            # 把回路的真实抖动掩盖掉。
            next_tick = now
        next_tick += period_ns
        if args.jitter_us:
            next_tick += random.randint(0, args.jitter_us * 1000)
        loop_ticks.append(time.monotonic_ns())
        tick += 1

        # 长跑时日志里必须有心跳，否则一小时的静默无法与"卡死"区分开。
        now = time.monotonic_ns()
        if now >= heartbeat_next:
            print(f"[{args.label or args.mode}] {(now - begin_ns) / 1e9:.0f}s "
                  f"命令={cmd_sent} 观测帧={observed_frames} 不稳定={unstable_reads} "
                  f"命令错误={cmd_errors} 未认出={cmd_unmatched} "
                  f"逐轴最大跟随误差=" +
                  ",".join(f"{value:.0f}" for value in axis_error_max) + " counts",
                  flush=True)
            heartbeat_next = now + HEARTBEAT_NS

        # ---- 1. 取最新一帧 ----
        frame, axes, received_ns, unstable = read_latest(obs, reader)
        if unstable:
            unstable_reads += 1
            continue
        observed_frames += 1
        obs_latency.append(received_ns - frame["mono_ns"])
        cycle = frame["cycle"]
        if previous_cycle is not None:
            cycle_delta_total += 1
            if cycle == previous_cycle + 1:
                cycle_delta_ok += 1
        previous_cycle = cycle
        if frame.get("flags", 0):
            flagged_frames += 1
        wkc = frame.get("wkc")
        wkc_histogram[wkc] = wkc_histogram.get(wkc, 0) + 1

        # ---- 2. 命令生效延迟：先前发过的目标值，在这一帧里出现了吗 ----
        observed_targets = tuple(axis["target_pos"] for axis in axes)
        # 队列一旦积压就再也追不回来（队首认不出，后面每一拍都只能丢队首），
        # 所以"第一次对不上"必须留下现场：是主站发的目标变了，还是客户端自己数错了。
        if pending and pending[0][1] != observed_targets and divergence_reports < 5:
            divergence_reports += 1
            print(f"[诊断] {(received_ns - begin_ns) / 1e9:.1f}s cycle={cycle} "
                  f"队首待认领={pending[0][1]} 观测目标={observed_targets} "
                  f"队列长度={len(pending)} 观测位置={tuple(a['pos'] for a in axes)}",
                  flush=True)
        if pending and pending[0][1] == observed_targets:
            send_ns, _ = pending.pop(0)
            delta = frame["mono_ns"] - send_ns
            if delta >= 0:
                cmd_latency.append(delta)
                cmd_matched += 1
            else:
                # 帧早于发送时刻：这是"目标值恰好和上一条相同"造成的假配对，
                # 计入未认出而不是当成负延迟。
                cmd_unmatched += 1
        while len(pending) > 4:
            pending.pop(0)
            cmd_unmatched += 1

        if send_every == 0 or tick % send_every != 0:
            continue

        # ---- 3. 算参考轨迹与目标 ----
        elapsed_s = (received_ns - begin_ns) / 1e9
        phase = 2.0 * math.pi * elapsed_s / args.period_s
        # 余弦往返：相位起点与终点速度都为零，且整周期回到起点，停机不会有台阶。
        swing = 0.5 * (1.0 - math.cos(phase))
        targets = []
        for index in range(axis_count):
            reference = start_positions[index] + args.amplitude_counts * swing
            error = reference - axes[index]["pos"]
            following_error.append(abs(error))
            if abs(error) > axis_error_max[index]:
                axis_error_max[index] = abs(error)
            correction = max(-args.clamp_counts, min(args.clamp_counts, args.kp * error))
            targets.append(int(round(reference + correction)))

        # ---- 4. 发回去，并把 ACK 读掉 ----
        # ACK 必须读：命令响应是实时周期线程用 MSG_DONTWAIT 发的，没人读就会把它
        # 顶成短写，然后连接被主站主动关掉（那正是 2.5b 修掉的那条路）。
        send_ns = time.monotonic_ns()
        try:
            cmd.sendall(b"set_external_target " +
                        " ".join(str(value) for value in targets).encode() + b"\n")
            ack = cmd_reader.line()
        except (OSError, ConnectionError) as exc:
            cmd_errors += 1
            print(f"命令通道在第 {cmd_sent} 条时报错并终止：{exc}", flush=True)
            break
        cmd_sent += 1
        if not ack.startswith("OK"):
            cmd_errors += 1
            print(f"命令被拒：{ack}", flush=True)
            if cmd_errors >= 3:
                break
            continue
        pending.append((send_ns, tuple(targets)))

    # ---- 汇总 ----
    span_ns = max(loop_ticks) - min(loop_ticks) if len(loop_ticks) > 1 else 0
    loop_gaps = [loop_ticks[i] - loop_ticks[i - 1] for i in range(1, len(loop_ticks))]

    obs_ms = scaled(summarise(obs_latency), 1e6, 3)
    cmd_ms = scaled(summarise(cmd_latency), 1e6, 3)
    result = {
        "label": args.label,
        "mode": args.mode,
        "requested_hz": args.hz,
        "read_hz": args.read_hz,
        "jitter_us": args.jitter_us,
        "seconds": args.seconds,
        "kp": args.kp,
        "observed_frames": observed_frames,
        "unstable_reads": unstable_reads,
        "achieved_read_hz": round(observed_frames / (span_ns / 1e9), 2) if span_ns else None,
        "read_interval_ms": scaled(summarise(loop_gaps), 1e6, 3),
        "observation_age_ms": obs_ms,
        "command_latency_ms": cmd_ms,
        "following_error_counts": summarise(following_error),
        "following_error_deg": scaled(summarise(following_error), COUNTS_PER_SHAFT_DEG, 4),
        "axis_error_max_counts": [round(value, 1) for value in axis_error_max],
        "cycle_delta_ok": cycle_delta_ok,
        "cycle_delta_total": cycle_delta_total,
        "flagged_frames": flagged_frames,
        "wkc_histogram": {str(key): value for key, value in wkc_histogram.items()},
        "cmd_sent": cmd_sent,
        "cmd_matched": cmd_matched,
        "cmd_unmatched": cmd_unmatched,
        "cmd_errors": cmd_errors,
    }

    print("", flush=True)
    print(f"=== 闭环客户端 [{args.label or args.mode}] ===", flush=True)
    print(f"观测帧 {observed_frames}，不稳定重问 {unstable_reads}，实测读取频率 "
          f"{result['achieved_read_hz']} Hz（请求 {args.read_hz}）", flush=True)
    if obs_ms["n"]:
        print(f"观测数据年龄(ms)  p50={obs_ms['p50']:.3f}  p90={obs_ms['p90']:.3f}  "
              f"p99={obs_ms['p99']:.3f}  max={obs_ms['max']:.3f}", flush=True)
    if cmd_ms["n"]:
        print(f"命令生效延迟(ms)  p50={cmd_ms['p50']:.3f}  p90={cmd_ms['p90']:.3f}  "
              f"p99={cmd_ms['p99']:.3f}  max={cmd_ms['max']:.3f}  "
              f"（{cmd_matched}/{cmd_sent} 条在观测里认出；分辨率一个主站周期）", flush=True)
    elif cmd_sent:
        print(f"命令生效延迟：{cmd_sent} 条发出，0 条在观测里认出（主站改写目标？）", flush=True)
    err_deg = result["following_error_deg"]
    if err_deg["n"]:
        print(f"跟随误差(counts)  p50={result['following_error_counts']['p50']:.0f}  "
              f"p99={result['following_error_counts']['p99']:.0f}  "
              f"max={result['following_error_counts']['max']:.0f}"
              f"  (≈{err_deg['max']:.3f} 度最大)", flush=True)
    if cycle_delta_total:
        print(f"cycle 连续性：相邻两帧差 1 的占 {cycle_delta_ok}/{cycle_delta_total} "
              f"= {100.0 * cycle_delta_ok / cycle_delta_total:.4f}%", flush=True)
    print(f"带标志帧 {flagged_frames}，WKC 分布 {result['wkc_histogram']}", flush=True)
    print("CL_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=False, indent=2)
    return 0 if observed_frames > 0 else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--obs-socket",
                        help="观测通道路径；不给则由 --deployment/EMASTER_DEPLOYMENT 推导")
    parser.add_argument("--cmd-socket", help="命令通道路径；closed 模式下不给则同上推导")
    parser.add_argument("--deployment", help="部署 ID，按主站的命名规则推导两个 socket 路径")
    parser.add_argument("--mode", choices=("readonly", "closed"), default="closed")
    parser.add_argument("--hz", type=float, default=50.0, help="策略频率（发目标的频率）")
    parser.add_argument("--read-hz", type=float, default=None,
                        help="读观测的频率，默认与 --hz 相同；调高它只提高测量分辨率，"
                             "不改变控制律")
    parser.add_argument("--jitter-us", type=int, default=0,
                        help="每次读取叠加的均匀抖动上界（微秒），用来把采样相位扫开")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--amplitude-counts", type=float, default=DEFAULT_AMPLITUDE_COUNTS,
                        help="余弦往返的峰值幅度，counts（默认 30 度）")
    parser.add_argument("--period-s", type=float, default=10.0, help="往返一个完整周期")
    parser.add_argument("--kp", type=float, default=0.0, help="位置反馈增益，0 即纯轨迹前馈")
    parser.add_argument("--clamp-counts", type=float, default=2000.0, help="反馈项限幅")
    parser.add_argument("--timeout", type=float, default=0.5, help="socket 超时（秒）")
    parser.add_argument("--label", default="")
    parser.add_argument("--out", help="把结果 JSON 写到这个路径")
    args = parser.parse_args()
    resolve_sockets(args, parser)
    if args.read_hz is None:
        args.read_hz = args.hz
    sys.exit(run(args))


if __name__ == "__main__":
    main()
