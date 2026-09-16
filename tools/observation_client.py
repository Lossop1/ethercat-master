#!/usr/bin/env python3
"""观测通道参考客户端。

存在的意义有两个，且都不是"给人看状态"（那是 tools/status_client 的活）：

1. **协议的可执行规格。** 主站侧的线格式写在 include/emaster/observation/wire.h 里，
   但"这份布局到底对不对"只有真的解一遍才知道。这个脚本按文档解 DUMP 的二进制流，
   并在每个字段上校验自洽性——它同时也是将来策略侧客户端的起手模板。
2. **对抗试验的工具。** 阶段 2 的判据里有一条是"连上就不读的客户端不得影响任何
   东西"，`--stall` 就是那条臂：连上、发一次 DUMP、然后永远不读。

用法：
    python3 observation_client.py INFO
    python3 observation_client.py HEAD
    python3 observation_client.py LATEST
    python3 observation_client.py DUMP --from 0 --count 64
    python3 observation_client.py WATCH --hz 50 --seconds 5
    python3 observation_client.py --stall
"""

import argparse
import socket
import struct
import sys
import time

# 与 include/emaster/observation/wire.h 一一对应。改动那里必须同步改这里，
# 而版本号是唯一的护栏：主站若升了版本，下面的 _decode_header 会直接拒绝而不是
# 按旧偏移解出错位的数字。
WIRE_VERSION = 1
WIRE_MAGIC = b"EO"
WIRE_KIND_FRAME = 1
WIRE_KIND_DUMP = 2
HEADER_BYTES = 56
AXIS_BYTES = 24
DUMP_HEADER_BYTES = 24
MAX_AXES = 16
# 与 wire.h 的 EMASTER_OBSERVATION_WIRE_MAX_DUMP_FRAMES 对应（= 环形缓冲槽数）。
MAX_DUMP_FRAMES = 256

# <  = 小端、无对齐填充。字段顺序与 wire.c 的偏移表逐条对应：
# magic[2] version[kind] frame_bytes axis_count flags wkc publish_index cycle
# mono_ns deadline_ns interval_ns
_HEADER = struct.Struct("<2sBBHHIiQQQQQ")
_AXIS = struct.Struct("<iiiiHHI")
# DUMP 事务头：magic[2] version kind header_bytes axis_count first_index frame_count frame_bytes
_DUMP = struct.Struct("<2sBBHHQII")

assert _HEADER.size == HEADER_BYTES, "头布局与 wire.h 不一致"
assert _AXIS.size == AXIS_BYTES, "轴布局与 wire.h 不一致"
assert _DUMP.size == DUMP_HEADER_BYTES, "DUMP 头布局与 wire.h 不一致"


class ProtocolError(Exception):
    """响应不符合协议：宁可直接失败，也不要按猜测解析出看似合理的数字。"""


class NeedMoreData(ProtocolError):
    """仅用于流式读取时的内部控制流：缓冲里的字节还不够，不是协议错。

    与真正的协议错分开，是因为两者的处置完全相反：字节不够要再收，协议错必须立刻
    停下。（C 侧把这两件事分给 decode_header 和 decode_frame 两个返回值，这里用
    异常层次表达同一件事。）
    """


def _recv_exactly(sock, count):
    """收满 count 字节。对端提前关闭时抛 ProtocolError。"""
    chunks = []
    remaining = count
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ProtocolError(f"连接在收满 {count} 字节前关闭（还差 {remaining}）")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_line(sock):
    """收一行文本响应（以 \\n 结尾），返回去掉换行的字符串。"""
    data = bytearray()
    while True:
        byte = sock.recv(1)
        if not byte:
            raise ProtocolError("连接在收到完整行之前关闭")
        if byte == b"\n":
            return data.decode("utf-8", errors="replace")
        data += byte


def send_verb(sock, verb):
    sock.sendall(verb.encode("ascii") + b"\n")


def decode_header(buffer, offset=0):
    """只解头。任何不自洽都抛 ProtocolError；字节不够抛 NeedMoreData。"""
    if len(buffer) - offset < HEADER_BYTES:
        raise NeedMoreData()
    (magic, version, kind, frame_bytes, axis_count, flags, wkc,
     publish_index, cycle, monotonic_ns, deadline_ns, interval_ns) = \
        _HEADER.unpack_from(buffer, offset)

    if magic != WIRE_MAGIC:
        raise ProtocolError(f"魔数不符：{magic!r}")
    if version != WIRE_VERSION:
        raise ProtocolError(f"线格式版本 {version} 不认识（本客户端支持 {WIRE_VERSION}）")
    if kind != WIRE_KIND_FRAME:
        raise ProtocolError(f"消息类型 {kind} 不是 FRAME")
    if axis_count > MAX_AXES:
        raise ProtocolError(f"轴数 {axis_count} 超过上限")
    expected = HEADER_BYTES + AXIS_BYTES * axis_count
    if frame_bytes != expected:
        raise ProtocolError(f"frame_bytes={frame_bytes} 与轴数 {axis_count} 不自洽"
                            f"（应为 {expected}）")
    return {
        "frame_bytes": frame_bytes,
        "axis_count": axis_count,
        "flags": flags,
        "wkc": wkc,
        "publish_index": publish_index,
        "cycle": cycle,
        "monotonic_ns": monotonic_ns,
        "deadline_ns": deadline_ns,
        "frame_interval_ns": interval_ns,
    }


def decode_frame(buffer, offset=0):
    """解一条完整的 FRAME 消息，返回 (frame_dict, 消耗字节数)。

    任何不自洽都抛异常，**不做部分信任**——这条通道喂的是控制策略，一个错位但
    "看起来合理"的关节角，比一次连接失败危险得多。
    """
    header = decode_header(buffer, offset)
    frame_bytes = header["frame_bytes"]
    axis_count = header["axis_count"]
    if len(buffer) - offset < frame_bytes:
        raise NeedMoreData()

    axes = []
    cursor = offset + HEADER_BYTES
    for _ in range(axis_count):
        (actual_position, target_position, actual_velocity, actual_torque,
         status_word, control_word, axis_flags) = _AXIS.unpack_from(buffer, cursor)
        axes.append({
            "actual_position": actual_position,
            "target_position": target_position,
            "actual_velocity": actual_velocity,
            "actual_torque": actual_torque,
            "status_word": status_word,
            "control_word": control_word,
            "flags": axis_flags,
        })
        cursor += AXIS_BYTES

    return {
        "publish_index": header["publish_index"],
        "cycle": header["cycle"],
        "monotonic_ns": header["monotonic_ns"],
        "deadline_ns": header["deadline_ns"],
        "frame_interval_ns": header["frame_interval_ns"],
        "wkc": header["wkc"],
        "flags": header["flags"],
        "axes": axes,
    }, frame_bytes


def decode_dump_header(buffer, offset=0):
    """解 DUMP 事务头。字节不够抛 NeedMoreData，不自洽抛 ProtocolError。"""
    if len(buffer) - offset < DUMP_HEADER_BYTES:
        raise NeedMoreData()
    (magic, version, kind, header_bytes, axis_count, first_index,
     frame_count, frame_bytes) = _DUMP.unpack_from(buffer, offset)

    if magic != WIRE_MAGIC:
        raise ProtocolError(f"DUMP 头魔数不符：{magic!r}")
    if version != WIRE_VERSION:
        raise ProtocolError(f"DUMP 头线格式版本 {version} 不认识")
    if kind != WIRE_KIND_DUMP:
        raise ProtocolError(f"消息类型 {kind} 不是 DUMP 事务头")
    if header_bytes < DUMP_HEADER_BYTES or header_bytes > len(buffer) - offset:
        raise ProtocolError(f"DUMP 头长度 {header_bytes} 不自洽")
    if axis_count > MAX_AXES:
        raise ProtocolError(f"DUMP 头轴数 {axis_count} 超过上限")
    if frame_bytes != HEADER_BYTES + AXIS_BYTES * axis_count:
        raise ProtocolError(f"DUMP 头 frame_bytes={frame_bytes} 与轴数 {axis_count} 不自洽")
    if frame_count > MAX_DUMP_FRAMES:
        raise ProtocolError(f"DUMP 头帧数 {frame_count} 超过环形容量")
    return {
        "header_bytes": header_bytes,
        "axis_count": axis_count,
        "first_index": first_index,
        "frame_count": frame_count,
        "frame_bytes": frame_bytes,
    }


def dump(sock, start, count):
    """取一段帧。返回 (first_index, frames)。

    **不假定**拿到的是请求的那么多：服务端在第一个空洞处停下，实际帧数由事务头给出。
    first_index 是服务端实际给的起点（请求越界时它被钳到可读窗口下界），与请求的 start
    分开返回，调用方才能判断"我要的窗口够不够"，而不是假设它一定满足。

    结束条件是事务头里的 frame_count，不是"对端关连接"——观测连接是长连接，服务端不会
    为一次 DUMP 关掉它。早先按"读到读不动为止"写的版本会永远卡在这里。
    """
    send_verb(sock, f"DUMP {start} {count}")
    buffer = _recv_exactly(sock, DUMP_HEADER_BYTES)
    header = decode_dump_header(buffer)
    buffer = buffer[header["header_bytes"]:]
    if header["axis_count"] == 0 and header["frame_count"] > 0:
        raise ProtocolError("DUMP 头声明有帧却零轴")

    frames = []
    for _ in range(header["frame_count"]):
        while len(buffer) < HEADER_BYTES:
            buffer += _recv_exactly(sock, HEADER_BYTES - len(buffer))
        frame_header = decode_header(buffer)
        if frame_header["axis_count"] != header["axis_count"]:
            raise ProtocolError(
                f"帧轴数 {frame_header['axis_count']} 与事务头 {header['axis_count']} 不一致")
        if len(buffer) < frame_header["frame_bytes"]:
            buffer += _recv_exactly(sock, frame_header["frame_bytes"] - len(buffer))
        frame, consumed = decode_frame(buffer)
        frames.append(frame)
        buffer = buffer[consumed:]

    if buffer:
        # 事务头说有几帧就收几帧，尾部不该再有东西。多出来的是服务端在流中间插了别的东西。
        raise ProtocolError(f"DUMP 事务之后还有 {len(buffer)} 字节未消费")
    return header["first_index"], frames


def describe(frame, index=None):
    """一行摘要。字段名与 status 响应保持可对照。"""
    prefix = f"[{index}] " if index is not None else ""
    axes = " ".join(
        f"a{n + 1}:pos={a['actual_position']},tgt={a['target_position']}"
        f",vel={a['actual_velocity']},tor={a['actual_torque']}"
        for n, a in enumerate(frame["axes"]))
    return (f"{prefix}cycle={frame['cycle']} pub={frame['publish_index']} "
            f"wkc={frame['wkc']} flags=0x{frame['flags']:08x} {axes}")


def connect(path, timeout):
    """连上并设置读超时。

    超时不是可有可无的礼貌：这个客户端由脚本调用，卡住与"还在跑"在外部看来
    完全一样，而脚本会一直等下去。给对方一个上界，失败就明确失败。
    """
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(path)
    return sock


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("verb", nargs="?", default="INFO",
                        choices=["INFO", "HEAD", "LATEST", "DUMP", "WATCH"])
    parser.add_argument("--socket", required=True, help="观测套接字路径")
    parser.add_argument("--from", dest="start", type=int, default=0)
    parser.add_argument("--count", type=int, default=16)
    parser.add_argument("--hz", type=float, default=50.0, help="WATCH 的拉取频率")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="单次 socket 读超时（秒）；超时即失败，绝不无限等待")
    parser.add_argument("--stall", type=int, nargs="?", const=1, default=0, metavar="N",
                        help="对抗臂：连发 N 条满窗口 DUMP（默认 1）后永不读取。"
                             "N 够大时能把套接字缓冲写满，把服务端的背压路径逼出来")
    args = parser.parse_args()

    sock = connect(args.socket, args.timeout)

    if args.stall:
        # 故意不回读。一条满窗口 DUMP 约 39 KB，而 AF_UNIX 缓冲通常有 200 KB 以上——
        # 只发一条的话写会一次性成功，压根碰不到服务端的重试/断开路径，这条臂就等于
        # 什么都没测。多发几条把缓冲填满，才谈得上"连上就不读会怎样"。
        for _ in range(args.stall):
            send_verb(sock, "DUMP 0 256")
        print(f"已发出 {args.stall} 条 DUMP 并停止读取，保持连接不读。", flush=True)
        try:
            # 先"真不读"一段：这段时间里服务端要么写得下（那就没触发背压），要么写不下
            # 并走完重试与总时限、把本连接关掉。**必须先不读再排空**——边读边等的话缓冲
            # 永远填不满，这条臂就退化成普通客户端，什么也证明不了。
            time.sleep(3.0)
            sock.setblocking(False)
            drained = 0
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    chunk = sock.recv(65536)
                except BlockingIOError:
                    time.sleep(0.05)
                    continue
                except OSError:
                    chunk = b""
                if not chunk:
                    print(f"服务端已断开本连接（对端已关，排空 {drained} 字节）：背压生效",
                          flush=True)
                    return 0
                drained += len(chunk)
            print(f"服务端未断开本连接（排空 {drained} 字节后仍在）：写缓冲容得下这条臂的量，"
                  f"加大 --stall 才能压到服务端的断开路径", flush=True)
            return 0
        except KeyboardInterrupt:
            return 0

    if args.verb in ("INFO", "HEAD", "LATEST"):
        send_verb(sock, args.verb)
        print(recv_line(sock))
        return 0

    if args.verb == "DUMP":
        first_index, frames = dump(sock, args.start, args.count)
        print(f"收到 {len(frames)} 帧（请求 {args.count}，服务端起点 {first_index}）")
        for n, frame in enumerate(frames):
            print(describe(frame, n))
        # 连续性自证：publish_index 逐帧 +1 才是无洞窗口。
        if frames:
            indices = [f["publish_index"] for f in frames]
            holes = sum(1 for a, b in zip(indices, indices[1:]) if b != a + 1)
            # 服务端报的起点必须与第一帧自报的序号一致，否则事务头和帧区对不上。
            if indices[0] != first_index:
                print(f"协议不一致：事务头起点 {first_index}，首帧序号 {indices[0]}")
                return 1
            print(f"publish_index 区间 [{indices[0]}, {indices[-1]}]，跳号 {holes} 处")
        return 0

    # WATCH：按客户端自己的频率取最新，主站不降频。
    period = 1.0 / args.hz
    deadline = time.monotonic() + args.seconds
    seen = 0
    last_cycle = None
    gaps = 0
    while time.monotonic() < deadline:
        started = time.monotonic()
        send_verb(sock, "LATEST")
        line = recv_line(sock)
        if line.startswith("OK|"):
            fields = dict(
                pair.split("=", 1) for pair in line[3:].split("|")[0].split(" ") if "=" in pair)
            cycle = int(fields.get("cycle", "0"))
            if last_cycle is not None and cycle > last_cycle + 1:
                gaps += 1
            last_cycle = cycle
            seen += 1
        sleep_for = period - (time.monotonic() - started)
        if sleep_for > 0:
            time.sleep(sleep_for)
    print(f"{args.seconds}s 内取到 {seen} 次，其中 cycle 跳号 {gaps} 次")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ProtocolError as error:
        print(f"协议错误：{error}", file=sys.stderr)
        sys.exit(1)
    except OSError as error:
        # 连接失败与读超时都走这里。超时是**明确的失败**，不是一个更慢的成功。
        print(f"通信错误：{type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(2)
