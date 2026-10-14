#!/usr/bin/env python3
"""设点通道的发送端（外部控制器这一侧的参考实现）。

主站的设点口是 AF_UNIX + **SOCK_SEQPACKET**、**只写不回**。这个工具照着
`include/emaster/setpoint/wire.h` 的记录格式打包发送：一条记录一次 send，发完就走。
它同时是 rl_sar 那一侧的实现样板——**协议在这两处各有一份实现**（这里与
`src/setpoint/wire.c`），改格式时一起改，`checks/setpoint_channel_check.py` 拿同一组
用例对两边。

用法（路径由部署 ID 推出来，与主站同一套命名）：

    # 保持在一组目标上，200 Hz 发 5 秒（位置是**原始计数**）
    python3 tools/setpoint_send.py --deployment bench12 --hold 1000 2000 --hz 200 --duration 5

    # 只发一条
    python3 tools/setpoint_send.py --deployment bench12 --once 1000 2000

    # 绕给定中心做正弦，幅度 30 度（要显式给每轴的 counts/度）
    python3 tools/setpoint_send.py --deployment bench12 --center 1000 2000 \\
        --sine-deg 30 --counts-per-degree 455.1 406.3 --hz 200 --duration 5

    # 不连主站，把会发出去的前几条打出来看一眼（离线核对波形与字节）
    python3 tools/setpoint_send.py --dry-run --hold 1000 2000 --axes 2 --hz 200

**位置是原始计数，不是度。** 换算属于上层：主站与观测通道都用同一套口径，这里多一层
换算是为了让 rl_sar 那侧少写一行，而不是给协议加一个单位。所以要做角度运动时
`--counts-per-degree` 必须显式给出，工具**不去猜**（猜错等于把轴按错的幅度驱动，
而打印出来的角度还是对的）。

**这条通道没有"就绪"握手。** 客户端连上就发，主站连上就收。唯一挡住"连上之前就发了
一个离谱的值"的是主站那条判据：目标离实际位置太远就拒收（它从第一拍起就在生效）。
台架上的用法是先等主站 state=4 再起这个工具。

**本工具会让电机按你给的目标动。** 涉及物理动作先确认机械与限位。
"""

import argparse
import math
import os
import socket
import struct
import sys
import time

# 与 include/emaster/bus/command_socket_path.h 的同名常量。两处必须一致。
DEPLOYMENT_ENV = "EMASTER_DEPLOYMENT"

MAGIC = b"ES"
WIRE_VERSION = 1
HEADER_BYTES = 24
AXIS_BYTES = 24
MAX_AXES = 16

# 头与每轴条目的字段布局，与 src/setpoint/wire.c 顶上的偏移表一致。
# "<2sBBHHQQ" = magic, version, reserved, record_bytes, axis_count, seq, client_time_ns
HEADER_LAYOUT = "<2sBBHHQQ"
# mode, flags, reserved, q, dq, tau, kp, kd
AXIS_LAYOUT = "<bBHi i i i i".replace(" ", "")

DEFAULT_MODE = 8  # CSP


def encode_record(sequence, client_time_ns, axes, mode=DEFAULT_MODE):
    """把一条记录打成字节。axes 是 [(q, dq, tau, kp, kd), ...]。

    逐字段 pack 而不是 pack 整个结构体：结构体的填充、对齐、字节序都是实现的事，
    把它当线格式等于把 ABI 焊进协议。这里与 C 侧是逐字节对应的两套代码，故意如此
    ——共有的是**格式**，不是同一份实现。
    """
    if not 1 <= len(axes) <= MAX_AXES:
        raise ValueError(f"轴数必须在 1..{MAX_AXES} 之间，收到 {len(axes)}")
    record_bytes = HEADER_BYTES + len(axes) * AXIS_BYTES
    out = bytearray()
    out += struct.pack(HEADER_LAYOUT, MAGIC, WIRE_VERSION, 0, record_bytes, len(axes),
                       sequence, client_time_ns)
    for q, dq, tau, kp, kd in axes:
        out += struct.pack(AXIS_LAYOUT, mode, 0, 0, q, dq, tau, kp, kd)
    assert len(out) == record_bytes, (len(out), record_bytes)
    return bytes(out)


def decode_record(data):
    """解一条记录。与本工具自己的编码互为对照，也是台架取证的读数路径。"""
    if len(data) < HEADER_BYTES:
        raise ValueError(f"不足一条头（{len(data)} < {HEADER_BYTES}）")
    magic, version, _reserved, record_bytes, axis_count, sequence, client_time = \
        struct.unpack_from(HEADER_LAYOUT, data, 0)
    if magic != MAGIC:
        raise ValueError(f"魔数不对：{magic!r}")
    if version != WIRE_VERSION:
        raise ValueError(f"版本不认识：{version}")
    if not 1 <= axis_count <= MAX_AXES:
        raise ValueError(f"轴数超出范围：{axis_count}")
    if record_bytes != HEADER_BYTES + axis_count * AXIS_BYTES:
        raise ValueError(f"record_bytes={record_bytes} 与轴数 {axis_count} 不自洽")
    if len(data) < record_bytes:
        raise ValueError(f"字节数不足：{len(data)} < {record_bytes}")
    axes = []
    for index in range(axis_count):
        axes.append(struct.unpack_from(AXIS_LAYOUT, data, HEADER_BYTES + index * AXIS_BYTES))
    return {"sequence": sequence, "client_time_ns": client_time, "axes": axes}


def setpoint_socket_path(deployment):
    """与 tools/emaster_endpoint.py 的 deployment_setpoint_socket 同一条规则。"""
    return f"/tmp/emaster-{deployment}-sp.sock"


def connect(path):
    """连设点口。**SOCK_SEQPACKET**，不是 SOCK_STREAM——消息边界由内核给。"""
    if not hasattr(socket, "AF_UNIX"):
        raise SystemExit(f"本机 Python 没有 AF_UNIX，连不了 {path}")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        sock.connect(path)
    except OSError as error:
        sock.close()
        raise SystemExit(f"连不上设点口 {path}：{error}\n"
                         f"（主站开了吗？设点通道要 EMASTER_SETPOINT=1 才建）") from None
    return sock


def parse_counts(text_list, what):
    values = []
    for text in text_list:
        for piece in text.split(","):
            piece = piece.strip()
            if not piece:
                continue
            try:
                values.append(int(piece, 0))
            except ValueError:
                raise SystemExit(f"{what} 必须是整数（原始计数）：{piece!r}") from None
            if not -2147483648 <= values[-1] <= 2147483647:
                raise SystemExit(f"{what} 超出 int32：{values[-1]}")
    return values


def weave(args):
    """按参数决定每拍各轴的目标，产出 [q, ...] 的无限序列。

    三种波形：

      once   —— 发一条就结束（连接后立刻关，主站在超时后保持）
      hold   —— 一直发同一组值
      sine   —— 绕 center 做正弦，幅度按每轴自己的 counts/度折算

    正弦是**绕中心**而不是从零开始：主站的目标是绝对计数，从零起振等于让轴先跳回
    原点。台架上每一轴的起点都不同，所以中心必须由调用方给（status 里读得到）。
    相位按已发条数推，不按墙上时钟——发送频率本来就由 --hz 定，两者用同一个源，
    波形频率才不会随调度抖动漂。
    """
    if args.once:
        yield parse_counts(args.once, "--once")
        return

    if args.hold:
        targets = parse_counts(args.hold, "--hold")
        while True:
            yield targets

    center = parse_counts(args.center, "--center")
    if len(center) != len(args.counts_per_degree):
        raise SystemExit(f"--center 给了 {len(center)} 轴，--counts-per-degree 给了 "
                         f"{len(args.counts_per_degree)} 轴，两者必须一一对应")
    amplitude = [args.sine_deg * factor for factor in args.counts_per_degree]
    step = args.sine_hz / args.hz
    index = 0
    while True:
        phase = 2 * math.pi * step * index
        yield [int(round(c + a * math.sin(phase))) for c, a in zip(center, amplitude)]
        index += 1


def build_parser():
    parser = argparse.ArgumentParser(
        description="设点通道发送端（只写，SOCK_SEQPACKET）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument("--deployment", default=None,
                        help=f"部署 ID；也可用 ${DEPLOYMENT_ENV}")
    parser.add_argument("--sp-socket", default=None, metavar="PATH",
                        help="直接给设点套接字路径（默认按部署 ID 推）")

    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--once", nargs="+", metavar="Q", help="只发一条记录，随即退出")
    mode.add_argument("--hold", nargs="+", metavar="Q", help="一直发这组目标")
    mode.add_argument("--sine-deg", type=float, metavar="DEG",
                      help="绕 --center 做正弦；要显式给 --counts-per-degree")

    parser.add_argument("--center", nargs="+", metavar="Q",
                        help="正弦的中心（每轴一个原始计数）")
    parser.add_argument("--counts-per-degree", nargs="+", type=float, metavar="F",
                        help="每轴 1 度等于多少 counts（输出轴）；正弦模式必须给")
    parser.add_argument("--sine-hz", type=float, default=0.5, help="正弦频率，默认 0.5 Hz")
    parser.add_argument("--hz", type=float, default=200.0, help="发送频率，默认 200 Hz")
    parser.add_argument("--duration", type=float, default=None, metavar="S",
                        help="持续秒数（--once 之外都要给）")
    parser.add_argument("--mode", type=int, default=DEFAULT_MODE,
                        help=f"逐轴运行模式，默认 {DEFAULT_MODE}（CSP）")
    parser.add_argument("--seq-start", type=int, default=1, help="首个序号，默认 1")
    parser.add_argument("--axes", type=int, default=None,
                        help="离线模式下的轴数（--dry-run 时用）")
    parser.add_argument("--dry-run", action="store_true",
                        help="不连主站，把头几条记录的内容与字节数打出来")
    parser.add_argument("--print-every", type=float, default=1.0, metavar="S",
                        help="每多少秒打一行进度，默认 1 秒；0 表示不打")
    return parser


def check_arguments(args, parser):
    if args.sine_deg is not None:
        if not args.center:
            parser.error("--sine-deg 必须配 --center（每轴一个原始计数）："
                         "目标绝对值从零起振会让轴先跳回原点")
        if not args.counts_per_degree:
            parser.error("--sine-deg 必须配 --counts-per-degree：这个工具不去猜换算，"
                         "猜错等于把轴按错的幅度驱动，而打印出来的角度还是对的")
        if len(args.center) != len(args.counts_per_degree):
            parser.error("--center 与 --counts-per-degree 的轴数必须一致")
    if args.once is None and args.duration is None:
        if args.dry_run:
            args.duration = 1.0  # 离线只看头几条，给个默认长度即可
        else:
            parser.error("--hold / --sine-deg 要给 --duration（秒）；--once 不需要")
    if args.hz <= 0:
        parser.error("--hz 必须为正")
    if args.mode < -128 or args.mode > 127:
        parser.error("--mode 要能放进 int8")


def dry_run(args, parser):
    """不连主站。把波形前几条与字节数打出来——离线核对格式与振幅用。"""
    if not args.axes:
        # 从波形本身推轴数，省得调用方再数一遍。
        args.axes = len(next(iter(weave(args))))
    if not 1 <= args.axes <= MAX_AXES:
        parser.error(f"--axes 必须在 1..{MAX_AXES}")
    period = 1.0 / args.hz
    count = 1 if args.once else int(round(args.duration * args.hz)) + 1
    print(f"[dry-run] 轴数={args.axes} 频率={args.hz} Hz 周期={period * 1000:.3f} ms "
          f"共 {count} 条 mode={args.mode}")
    generator = weave(args)
    for index in range(min(count, 5)):
        targets = next(generator)
        if len(targets) != args.axes:
            parser.error(f"波形给了 {len(targets)} 轴，--axes 是 {args.axes}，两者必须一致")
        axes = [(q, 0, 0, 0, 0) for q in targets]
        data = encode_record(args.seq_start + index, 0, axes, args.mode)
        back = decode_record(data)
        print(f"[dry-run] seq={args.seq_start + index} {len(data)} 字节 "
              f"q={[axis[3] for axis in back['axes']]} 往返一致")
    print(f"[dry-run] 每条 {HEADER_BYTES + args.axes * AXIS_BYTES} 字节；"
          f"没有连主站，没有发任何东西")
    return 0


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    check_arguments(args, parser)

    if args.dry_run:
        return dry_run(args, parser)

    path = args.sp_socket
    if path is None:
        deployment = args.deployment or os.environ.get(DEPLOYMENT_ENV)
        if not deployment:
            parser.error(f"要给 --deployment（或用 --sp-socket 直接给路径，"
                         f"或设 {DEPLOYMENT_ENV}）")
        path = setpoint_socket_path(deployment)

    sock = connect(path)
    period = 1.0 / args.hz
    deadline = None if args.once else time.monotonic() + args.duration
    generator = weave(args)
    sequence = args.seq_start
    sent = 0
    next_report = time.monotonic() + args.print_every if args.print_every > 0 else None
    started = time.monotonic()
    # client_time_ns 由本工具填，主站只原样记下。用单调时钟，与主站自己的时钟无关。
    origin = time.monotonic_ns()

    try:
        while True:
            now = time.monotonic()
            if deadline is not None and now > deadline:
                break
            targets = next(generator)
            axes = [(q, 0, 0, 0, 0) for q in targets]
            record = encode_record(sequence, time.monotonic_ns() - origin, axes, args.mode)
            try:
                sock.sendall(record)
            except OSError as error:
                print(f"[setpoint] 发送失败（第 {sequence} 号）：{error}", file=sys.stderr)
                return 1
            sent += 1
            sequence += 1
            if next_report is not None and now >= next_report:
                print(f"[setpoint] 已发 {sent} 条，最新 seq={sequence - 1} "
                      f"q={targets}", flush=True)
                next_report += args.print_every
            if args.once:
                break
            # 按绝对时刻对齐，不是每次 sleep 一个周期——后者会把发送频率拖着往下走，
            # 而"客户端到底多少赫兹"正是报告里要用序号去判的东西。
            target_moment = started + sent * period
            slack = target_moment - time.monotonic()
            if slack > 0:
                time.sleep(slack)
    finally:
        sock.close()

    span = time.monotonic() - started
    rate = sent / span if span > 0 else 0.0
    print(f"[setpoint] 发送结束：{sent} 条，用时 {span:.3f} s（约 {rate:.1f} Hz），"
          f"末号 seq={sequence - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
