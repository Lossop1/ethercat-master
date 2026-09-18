#!/usr/bin/env python3
"""把主站的两条通道包成能嵌进界面的对象，协议实现一律转发给已有的件。

**为什么要有这一层。** 两条通道的完整实现仓库里都有，但它们是以"命令行脚本"的
姿态写的：

- 命令通道：tools/interactive_control.py 的 EtherCATClient（协议 + 4 秒空闲重连），
  tools/emaster_console.py 的 ConsoleClient 又在上面加了"不往 stdout 打印"和
  last_problem/last_response 两个诊断字段。emaster_console.Engine 依赖的正是
  ConsoleClient 这个接口，所以界面直接用 ConsoleClient，不另造一个。
- 观测通道：tools/observation_client.py 的解码器（与 wire.h 一一对应，带自洽校验），
  tools/closed_loop_client.py 的 LineReader 与 parse_latest。

界面要的是"能放进后台线程被反复驱动、并且把异常如实报出来"的对象，不是脚本。
所以这里不做第三份协议实现——解码、行读取、解析全部转发给上面那些已经验过的函数。
新增的只有三样：

1. ObservationClient：把观测通道的四个动词包成一个对象，并管住"行读取器可能多吃了
   一截"这件事（见 _FramedSocket）。
2. 回复分类：命令通道的回复前缀层数**不一致**。set_external_target 分支自己就往
   message 里写了 "OK|"（session_control.c:1028），command_server.c:528 再包一层，
   于是线上是 "OK|OK|Updated 2 external targets"；而队列满是单层的
   "ERROR|Command queue full"。所以判据只能是"以 OK 开头 / 以 ERROR 开头"，
   **不能按第一个竖线切**。
3. 标志位表：观测帧的轴级/帧级标志（include/emaster/observation/frame.h）。

**取数的语义坑（写进界面之前必须知道）：**

- HEAD 的 `newest` 是**开区间上界**（ring.c:155 写的就是 head）。可读窗口是
  [head-capacity+1, head)。最后已发布的那一帧是 `newest - 1`。区间算错一格就会
  持续漏帧，而且看起来"一切正常"。
- ring_capacity 是**槽数**，1 kHz 下就是 256 ms 的历史。取数跟不上就永久丢，
  没有丢帧通知，只能靠 `publish_index` 跳号看出来。
- LATEST 的轴字段是 pos/vel/torque/status/target_pos/control/flags，**没有**
  planned/err/state/mode（那几个只在命令通道的 status 里）。所以"轴表"要那两个
  来源一起用：高频量取观测，控制状态量取 status。

判定一律用 counts，不做浮点换算——与观测通道自己的约定一致，免得到处是浮点常数。
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emaster_endpoint  # noqa: E402
import observation_client as obs_wire  # noqa: E402
from closed_loop_client import LineReader, parse_latest  # noqa: E402

# ---------------------------------------------------------------- 标志位

# 与 include/emaster/observation/frame.h 的枚举逐位对应。改那里必须同步改这里，
# 位号写错不会报错，只会安静地把"无速度反馈"显示成别的东西。
FRAME_FLAGS = (
    (1 << 0, "WKC不符"),
    (1 << 1, "整帧未回"),
    (1 << 2, "错过截止期"),
    (1 << 3, "跳拍"),
)

AXIS_FLAGS = (
    (1 << 0, "WKC不符"),
    (1 << 1, "整帧未回"),
    (1 << 2, "解码不全"),
    (1 << 3, "已隔离"),
    (1 << 4, "目标未知"),
    (1 << 5, "无速度反馈"),
    (1 << 6, "无力矩反馈"),
)

# 这两个要单独判：它们不是"出问题了"，是"这套 PDO 映射里根本没有这个量"。
# 此时 vel/torque 恒为 0，显示成 0 就是把常量当真值。
AXIS_FLAG_VELOCITY_UNAVAILABLE = 1 << 5
AXIS_FLAG_TORQUE_UNAVAILABLE = 1 << 6

# 命令通道的主站状态机（docs/protocol/command-protocol-v1.md）。
MASTER_STATES = {
    0: "初始化", 1: "预运行", 2: "安全运行", 3: "使能中",
    4: "运行中", 5: "停机中", 6: "故障",
}


def flag_names(flags, table):
    """把一组标志位翻译成人话。未定义的位原样报出来，不吞掉。"""
    names = [name for bit, name in table if flags & bit]
    known = 0
    for bit, _ in table:
        known |= bit
    leftover = flags & ~known
    if leftover:
        names.append(f"未知位0x{leftover:08x}")
    return names


def axis_flags_text(flags):
    return "、".join(flag_names(flags, AXIS_FLAGS)) or "—"


def frame_flags_text(flags):
    return "、".join(flag_names(flags, FRAME_FLAGS)) or "—"


# ---------------------------------------------------------------- 回复分类

def reply_kind(reply):
    """ok / error / unknown / none。

    **不按第一个竖线切**，理由见模块开头：前缀层数在不同分支上不一致。
    """
    if reply is None:
        return "none"
    if reply.startswith("OK"):
        return "ok"
    if reply.startswith("ERROR"):
        return "error"
    return "unknown"


def reply_text(reply):
    """去掉重复的前缀，只留人要看的那句话（双前缀 → 单层）。"""
    text = reply or ""
    for _ in range(8):          # 上界只是防呆，实际最多两层
        stripped = False
        for prefix in ("OK|", "ERROR|"):
            if text.startswith(prefix):
                text = text[len(prefix):]
                stripped = True
        if not stripped:
            break
    return text


def truncated(reply):
    """status 响应被缓冲区截断了吗。

    轴数多到填满 message 时，主站把最后 8 字节写成 |TRUNC
    （session_control.c:774-777）。读到它就不能把这组轴数据当完整快照用。
    """
    return bool(reply) and "|TRUNC" in reply


# ---------------------------------------------------------------- 观测通道

# 这条连接不能再用了的异常集合。ProtocolError 也在这里：它意味着字节流已经对不上
# 位了（魔数/版本/长度不自洽），继续在同一条连接上问下去只会拿到错位的数字——
# 而这条通道的数字是要拿去做判断的，错位比断开危险得多。
CHANNEL_ERRORS = (OSError, obs_wire.ProtocolError)


class _FramedSocket:
    """给 observation_client.dump() 用的 socket 替身。

    dump() 按字节数收（_recv_exactly），而文本动词走 LineReader（recv(4096)）。
    两者共用一条连接，所以行读取器的缓冲里理论上可能已经多吞了一截。正常不会——
    观测协议是一条请求一条响应，服务端不会抢跑——但"不会发生"和"发生了也不出错"
    是两回事，这里先把那截交出去，而不是假设它一定是空的。
    """

    def __init__(self, sock, reader):
        self._sock = sock
        self._reader = reader

    def sendall(self, data):
        self._sock.sendall(data)

    def recv(self, count):
        buffered = self._reader.buffer
        if buffered:
            take = min(count, len(buffered))
            chunk = bytes(buffered[:take])
            del buffered[:take]
            return chunk
        return self._sock.recv(count)


class ObservationClient:
    """观测通道。只读——它连的那条口子本身就只能读。

    一次一条动词、读到一整行再发下一条。这条通道是拉取式的，服务端一条请求一条
    响应，没有流水线的余地。
    """

    def __init__(self, endpoint, timeout=2.0):
        self.endpoint = emaster_endpoint.Endpoint.parse(endpoint)
        self.timeout = timeout
        self.sock = None
        self._reader = None
        self._framed = None

        # INFO 带下来的静态元数据。连上后读一次即可，会话内不变。
        self.info_fields = {}
        self.deployment_id = None
        self.interface_name = None
        self.axis_count = 0
        self.stride = 1
        self.ring_capacity = 0
        self.wire_version = None

    @property
    def connected(self):
        return self.sock is not None

    @property
    def display(self):
        return self.endpoint.display

    def connect(self):
        self.disconnect()
        sock = self.endpoint.connect(self.timeout)
        sock.settimeout(self.timeout)
        self.sock = sock
        self._reader = LineReader(sock)
        self._framed = _FramedSocket(sock, self._reader)
        return True

    def disconnect(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self._reader = None
        self._framed = None

    def _verb(self, text):
        if self.sock is None:
            raise ConnectionError("观测通道没连上")
        self.sock.sendall(text.encode("ascii") + b"\n")
        return self._reader.line()

    @staticmethod
    def _fields(line):
        """把 "OK|k=v|k=v" 解成字典。顶层字段之间是竖线，没有空格。"""
        out = {}
        for part in line.split("|")[1:]:
            key, sep, value = part.partition("=")
            if sep:
                out[key.strip()] = value.strip()
        return out

    def read_info(self):
        """INFO：读静态元数据并记下来。返回原始那一行（诊所里用得上）。"""
        line = self._verb("INFO")
        fields = self._fields(line)
        self.info_fields = fields
        self.deployment_id = fields.get("deployment")
        self.interface_name = fields.get("interface")
        self.wire_version = fields.get("wire_version")
        self.axis_count = int(fields.get("axes", "0") or 0)
        self.stride = int(fields.get("stride", "1") or 1)
        self.ring_capacity = int(fields.get("capacity", "0") or 0)
        return line

    def read_head(self):
        """HEAD：返回 (oldest, newest, capacity, empty)。

        **newest 是开区间上界**：可读的是 [oldest, newest)，最后已发布的那一帧是
        newest - 1。还没发布过任何帧时 empty=True，其余三个都是 0。
        """
        line = self._verb("HEAD")
        fields = self._fields(line)
        if fields.get("empty") == "1" or "oldest" not in fields:
            return 0, 0, int(fields.get("capacity", "0") or 0), True
        return (int(fields["oldest"]), int(fields["newest"]),
                int(fields.get("capacity", "0") or 0), False)

    def read_latest(self):
        """LATEST：返回 (frame, axes)。取不到或解析不动时是 (None, None)。

        两种情况会拿不到：环还是空的（OK|empty=1），或者写者正好越过那一格
        （ERROR|unstable）——后者重问一次就有，不是错误。
        """
        line = self._verb("LATEST")
        return parse_latest(line)

    def read_window(self, start, count):
        """DUMP：取一段帧，返回 (first_index, frames)。

        **不假定**拿到请求的那么多：服务端在第一个空洞处停下，实际帧数由事务头
        给出；start 越界时被钳到可读下界。调用方要拿 first_index 与帧的
        publish_index 自己判断窗口连不连续，不能假设它一定满足。
        """
        return obs_wire.dump(self._framed, start, count)


# ---------------------------------------------------------------- 自查

def _main():
    """不经过界面地走一遍观测通道，用来单独验证桥/套接字通不通。

    python3 tools/emaster_client.py --deployment orangepi-bench-quint-30deg
    python3 tools/emaster_client.py --tcp 192.168.124.81:5001
    """
    ap = argparse.ArgumentParser(description="观测通道自查（INFO / HEAD / 一个窗口）")
    emaster_endpoint.add_endpoint_arguments(ap, command=False, observation=True)
    ap.add_argument("--count", type=int, default=64, help="取多少帧（默认 64）")
    args = ap.parse_args()

    endpoints = emaster_endpoint.resolve_endpoints(args, ap, need=("observation",))
    client = ObservationClient(endpoints["observation"])
    try:
        client.connect()
        print("端点：", client.display)
        print("INFO：", client.read_info())
        oldest, newest, capacity, empty = client.read_head()
        if empty:
            print("HEAD：环还是空的（主站还没发布过帧）")
            return 0
        print(f"HEAD：可读 [{oldest}, {newest})，容量 {capacity}，"
              f"共 {newest - oldest} 帧可读")
        start = max(oldest, newest - args.count)
        first, frames = client.read_window(start, args.count)
        if not frames:
            print("窗口里一帧都没有")
            return 1
        indices = [f["publish_index"] for f in frames]
        holes = sum(1 for a, b in zip(indices, indices[1:]) if b != a + 1)
        if indices[0] != first:
            print(f"协议不一致：事务头起点 {first}，首帧序号 {indices[0]}")
            return 1
        span_s = (frames[-1]["monotonic_ns"] - frames[0]["monotonic_ns"]) / 1e9
        print(f"取到 {len(frames)} 帧，publish_index [{indices[0]}, {indices[-1]}]，"
              f"跳号 {holes} 处，跨 {span_s:.3f} 秒")
        last = frames[-1]
        print(f"末帧：cycle={last['cycle']} wkc={last['wkc']} "
              f"flags={frame_flags_text(last['flags'])} "
              f"帧距={last['frame_interval_ns'] / 1e6:.3f} ms")
        for n, axis in enumerate(last["axes"]):
            print(f"  轴{n + 1}: pos={axis['actual_position']} "
                  f"target={axis['target_position']} "
                  f"vel={axis['actual_velocity']} torque={axis['actual_torque']} "
                  f"status=0x{axis['status_word']:04x} "
                  f"[{axis_flags_text(axis['flags'])}]")
        return 0
    except CHANNEL_ERRORS as exc:
        print(f"观测通道出错：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    finally:
        client.disconnect()


if __name__ == "__main__":
    sys.exit(_main())
