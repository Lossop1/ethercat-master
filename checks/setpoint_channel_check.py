#!/usr/bin/env python3
"""设点通道的结构前提，离线验。

    python3 checks/setpoint_channel_check.py
    python3 checks/setpoint_channel_check.py --mutate   # 证明这检查抓得住改回去

设点口写进的是周期路径唯一的输入，而它面向的客户端（rl_sar 那侧）与主站**不共享任何
代码**：格式在 C 与 Python 各有一份实现，靠约定对齐。约定是会漂的，所以这里把"两份
实现对得上"变成一条会红的检查，而不是一条注释。

判五件事：

  1. **两份实现逐字节一致。** 从 `wire.h` / `wire.c` 里读出头与每轴条目的**偏移表**，
     再用 Python 那份实现真编一条记录，逐字节核对落在同一批位置上。改任何一边的字段
     顺序、宽度、对齐，这里就红——错位的目标值会被主站当成合法值写进输出路径，
     远比连不上危险。
  2. **魔数与版本真的被校验。** 光有常量不叫校验：解码器必须比对这两个字段，且
     `record_bytes` 要与 `axis_count` 自洽。少任何一条，旧客户端灌进来的字节会被
     按错位解释，而解出来的数看起来仍然合理。
  3. **套接字是 SOCK_SEQPACKET。** 换成 SOCK_STREAM 就把"一条记录"退化成字节流，
     粘包与半条记录这一整类问题立刻回来，而台架上表现为"偶发一个离谱的目标"。
  4. **链路上没有回写。** 服务端一个 send/write 都没有，客户端工具也不收。
     这条通道一旦在某处等客户端，"客户端多久发一次"就变成了主站的抖动来源。
  5. 接收缓冲是**最大记录 +1**：多出来的那一个字节是超长记录的判据（SOCK_SEQPACKET
     截断后回满长度）。少一个字节，超长消息的尾部会被当成合法记录采纳。

Python 那份实现是**载进内存跑的**：变异对照只改内存里的副本，绝不碰仓库文件。
"""

import os
import re
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIRE_H = "include/emaster/setpoint/wire.h"
WIRE_C = "src/setpoint/wire.c"
SERVER_H = "include/emaster/setpoint/server.h"
SERVER_C = "src/setpoint/server.c"
TOOL = "tools/setpoint_send.py"

# 头与每轴条目的偏移，按线上顺序写死。这是**协议**里最不该动的一列数字：
# 动它必须同时动版本号与对端实现。
HEADER_OFFSETS = {
    "MAGIC": 0, "VERSION": 2, "RESERVED": 3, "RECORD_BYTES": 4,
    "AXIS_COUNT": 6, "SEQUENCE": 8, "CLIENT_TIME_NS": 16,
}
AXIS_OFFSETS = {"MODE": 0, "FLAGS": 1, "Q": 4, "DQ": 8, "TAU": 12, "KP": 16, "KD": 20}
HEADER_BYTES = 24
AXIS_BYTES = 24
MAX_AXES = 16

# 模式核对那一句。它的**位置**也判：核在写入之前，模式不匹配的记录一个字节都不进汇点。
MODE_GUARD = "if (server->expected_modes[axis_index] != EMASTER_SETPOINT_MODE_UNCHECKED &&"
STORE_CALL = "store_record(server, &record);"

PASSED = 0
FAILED = 0


def ok(what):
    global PASSED
    PASSED += 1
    print(f"ok    {what}")


def bad(what, expected, actual):
    global FAILED
    FAILED += 1
    print(f"FAIL  {what}\n      期望：{expected}\n      实际：{actual}")


def strip_comments(text):
    """去注释，只留代码与字符串字面量。偏移表在注释里也抄了一遍，数原始文本会喂饱断言。"""
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char in ('"', "'"):
            quote = char
            out.append(char)
            index += 1
            while index < length and text[index] != quote:
                if text[index] == "\\":
                    out.append(text[index])
                    index += 1
                if index < length:
                    out.append(text[index])
                    index += 1
            if index < length:
                out.append(text[index])
                index += 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
        elif text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
        else:
            out.append(char)
            index += 1
    return "".join(out)


def read(root, relative, strip=True):
    try:
        with open(os.path.join(root, relative), encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError:
        return ""
    # Python 的注释是 '#'，这个去注释器不认；载进内存的是源码本身，不能动。
    return strip_comments(text) if strip else text


def define(text, name):
    """取一个 #define 的整数值。取不到返回 None。"""
    match = re.search(r"#define\s+" + re.escape(name) + r"\s+(\d+)U?\b", text)
    return int(match.group(1)) if match else None


def define_char(text, name):
    """取一个 #define 的字符值（`((uint8_t)'E')` 这种）。取不到返回 None。"""
    match = re.search(r"#define\s+" + re.escape(name) + r"\s+.*?'(.)'", text)
    return ord(match.group(1)) if match else None


def load_tool(source_text):
    """把 tools/setpoint_send.py 的**源码**载成一个模块——那是协议的另一份实现。

    走 exec 而不是 importlib：变异对照要拿改过的源码跑同一批断言，而 import 只会
    拿到进程里缓存的第一份。
    """
    module = types.ModuleType("setpoint_send_under_test")
    module.__file__ = TOOL
    exec(compile(source_text, TOOL, "exec"), module.__dict__)  # noqa: S102  （就是它本身）
    return module


def check_geometry(files, tool):
    """第 1 件：常量与偏移表，两边一致。"""
    wire_h = files[WIRE_H]
    wire_c = files[WIRE_C]

    for name, want, what in (
        ("EMASTER_SETPOINT_WIRE_HEADER_BYTES", HEADER_BYTES, "头字节数"),
        ("EMASTER_SETPOINT_WIRE_AXIS_BYTES", AXIS_BYTES, "每轴字节数"),
        ("EMASTER_SETPOINT_WIRE_MAX_AXES", MAX_AXES, "轴数上限"),
    ):
        found = define(wire_h, name)
        if found == want:
            ok(f"wire.h 的{what}是 {want}")
        else:
            bad(f"wire.h 的{what}是 {want}", want, found)

    for name, got, want in (("HEADER_BYTES", tool.HEADER_BYTES, HEADER_BYTES),
                            ("AXIS_BYTES", tool.AXIS_BYTES, AXIS_BYTES),
                            ("MAX_AXES", tool.MAX_AXES, MAX_AXES)):
        if got == want:
            ok(f"工具里的 {name} 也是 {want}")
        else:
            bad(f"工具里的 {name} 是 {want}", want, got)

    magic_0 = define_char(wire_h, "EMASTER_SETPOINT_WIRE_MAGIC_0")
    magic_1 = define_char(wire_h, "EMASTER_SETPOINT_WIRE_MAGIC_1")
    if (magic_0, magic_1) == (ord("E"), ord("S")) and tool.MAGIC == b"ES":
        ok("魔数两边都是 'E','S'")
    else:
        bad("魔数两边都是 'E','S'", "wire.h=('E','S') 工具=b'ES'",
            f"wire.h=({magic_0},{magic_1}) 工具={tool.MAGIC!r}")

    version = define(wire_h, "EMASTER_SETPOINT_WIRE_VERSION")
    if version is not None and version == tool.WIRE_VERSION:
        ok(f"版本号两边都是 {version}")
    else:
        bad("版本号两边一致", f"wire.h=1 工具={tool.WIRE_VERSION}", f"wire.h={version}")

    # C 侧的偏移表：wire.c 顶上那批 #define 是唯一可执行的真相（注释里那份不算）。
    wanted = {f"REC_OFF_{k}": v for k, v in HEADER_OFFSETS.items()}
    wanted.update({f"AX_OFF_{k}": v for k, v in AXIS_OFFSETS.items()})
    for name, want in wanted.items():
        found = define(wire_c, name)
        if found == want:
            ok(f"{name} = {want}")
        else:
            bad(f"{name} = {want}", want, found)

    # 末字段加宽度必须正好铺满——差一字节说明有填充或漏了字段，而填充位在两边会不一样。
    tail = AXIS_OFFSETS["KD"] + 4
    if tail == AXIS_BYTES:
        ok("最后一个轴字段（kd）正好铺满 24 字节，线上无填充")
    else:
        bad("轴条目无填充", f"kd 结束于 {AXIS_BYTES}", f"结束于 {tail}")
    tail = HEADER_OFFSETS["CLIENT_TIME_NS"] + 8
    if tail == HEADER_BYTES:
        ok("头最后一个字段（client_time_ns）正好铺满 24 字节")
    else:
        bad("头无填充", f"client_time_ns 结束于 {HEADER_BYTES}", f"结束于 {tail}")

    # 真编一条，逐字节核对两边落在同一批位置上。上面比的是常量，这里比的是**行为**：
    # struct.pack 的布局写错、字节序写错，只有到这一步才会露头。
    record_bytes = HEADER_BYTES + 2 * AXIS_BYTES
    wanted_bytes = {
        HEADER_OFFSETS["MAGIC"]: b"ES",
        HEADER_OFFSETS["VERSION"]: bytes([tool.WIRE_VERSION]),
        HEADER_OFFSETS["RESERVED"]: b"\x00",
        HEADER_OFFSETS["RECORD_BYTES"]: record_bytes.to_bytes(2, "little"),
        HEADER_OFFSETS["AXIS_COUNT"]: (2).to_bytes(2, "little"),
        HEADER_OFFSETS["SEQUENCE"]: (0x0102030405060708).to_bytes(8, "little"),
        HEADER_OFFSETS["CLIENT_TIME_NS"]: (0x1112131415161718).to_bytes(8, "little"),
        HEADER_BYTES + AXIS_OFFSETS["MODE"]: bytes([(-5) & 0xFF]),
        HEADER_BYTES + AXIS_OFFSETS["FLAGS"]: b"\x00",
        HEADER_BYTES + AXIS_OFFSETS["Q"]: (0x01020304).to_bytes(4, "little"),
        HEADER_BYTES + AXIS_OFFSETS["DQ"]: ((-2) & 0xFFFFFFFF).to_bytes(4, "little"),
        HEADER_BYTES + AXIS_OFFSETS["TAU"]: (3).to_bytes(4, "little"),
        HEADER_BYTES + AXIS_OFFSETS["KP"]: ((-4) & 0xFFFFFFFF).to_bytes(4, "little"),
        HEADER_BYTES + AXIS_OFFSETS["KD"]: (5).to_bytes(4, "little"),
    }
    try:
        data = tool.encode_record(0x0102030405060708, 0x1112131415161718,
                                  [(0x01020304, -2, 3, -4, 5), (7, 8, 9, 10, 11)], mode=-5)
        problems = [f"偏移 {offset}：期望 {want.hex()} 实际 {data[offset:offset + len(want)].hex()}"
                    for offset, want in sorted(wanted_bytes.items())
                    if data[offset:offset + len(want)] != want]
        if len(data) != record_bytes:
            problems.append(f"整条 {len(data)} 字节，应为 {record_bytes}")
        if not problems:
            ok(f"Python 编出的 {len(data)} 字节，逐字段落在 C 侧那张偏移表上")
        else:
            bad("Python 编出的记录逐字段落在 C 侧偏移表上", "全部相符", problems)
        # 解出来的是 (mode, flags, 保留, q, dq, tau, kp, kd)，与线上顺序一致。
        back = tuple(tool.decode_record(data)["axes"][0])
        if back == (-5, 0, 0, 0x01020304, -2, 3, -4, 5):
            ok("Python 自己的解码与编码互为逆（含负数与高位字节）")
        else:
            bad("Python 解码与编码互为逆", "(-5,0,0,0x01020304,-2,3,-4,5)", back)
    except Exception as error:  # noqa: BLE001  （编不出来本身就是这一项不符）
        bad("Python 侧能把一条记录编出来", "无异常", repr(error))


def check_validation(files):
    """第 2 件：魔数、版本、长度自洽，解码器里真的有这几道比对。"""
    wire_c = files[WIRE_C]

    for what, needle in (
        ("比对魔数", "buffer[REC_OFF_MAGIC] != EMASTER_SETPOINT_WIRE_MAGIC_0"),
        ("比对版本", "buffer[REC_OFF_VERSION] != (uint8_t)EMASTER_SETPOINT_WIRE_VERSION"),
        ("查轴数在 1..上限内",
         "axis_count == 0U || axis_count > (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES"),
        ("查长度与轴数自洽", "record_bytes != emaster_setpoint_wire_record_bytes(axis_count)"),
        ("查实收字节数够", "length < (size_t)record_bytes"),
    ):
        if needle in wire_c:
            ok(f"解码器{what}")
        else:
            bad(f"解码器{what}", needle, "没找到")

    # 编码器也得查容量：容量不足时不许写 *written，否则调用方会当真写成功。
    if "capacity < total" in wire_c:
        ok("编码器容量不足时返回 false 且不动 *written")
    else:
        bad("编码器容量不足时返回 false", "capacity < total", "没找到")


def check_socket(files):
    """第 3 件：SOCK_SEQPACKET，backlog 1，新连接顶掉旧的，缓冲留一个判超长的字节。"""
    server_c = files[SERVER_C]

    if "SOCK_SEQPACKET" in server_c and "SOCK_STREAM" not in server_c:
        ok("服务端用 SOCK_SEQPACKET（全文件没有 SOCK_STREAM）")
    else:
        bad("服务端用 SOCK_SEQPACKET", "有 SOCK_SEQPACKET、无 SOCK_STREAM",
            f"SEQPACKET={'SOCK_SEQPACKET' in server_c} STREAM={'SOCK_STREAM' in server_c}")

    if "#define SETPOINT_RX_CAPACITY (EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES + 1U)" in server_c:
        ok("接收缓冲是最大记录 +1（那一个字节就是超长记录的判据）")
    else:
        bad("接收缓冲是最大记录 +1",
            "#define SETPOINT_RX_CAPACITY (EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES + 1U)",
            "没找到")
    if "length >= (size_t)SETPOINT_RX_CAPACITY" in server_c and \
            "EMASTER_SETPOINT_REJECT_TRUNCATED" in server_c:
        ok("收满缓冲即判超长并整条拒收，不解析被截断的尾部")
    else:
        bad("收满缓冲即判超长", "length >= SETPOINT_RX_CAPACITY → REJECT_TRUNCATED", "没找到")

    if "listen(server->listen_fd, 1)" in server_c:
        ok("listen backlog 是 1（单客户端模型）")
    else:
        bad("listen backlog 是 1", "listen(server->listen_fd, 1)", "没找到")

    # 新连接顶掉旧连接：两个客户端同时写同一个汇点没有仲裁依据。
    positions = (server_c.find("drop_client(server);"),
                 server_c.find("server->client_fd = new_fd;"))
    if 0 <= positions[0] < positions[1]:
        ok("accept 时先断开旧客户端再接管新的")
    else:
        bad("accept 时先断开旧客户端再接管新的",
            "先 drop_client(server) 再 client_fd = new_fd", str(positions))

    # 逐轴模式核对要在写进汇点之前：模式对不上时同一条记录语义完全不同。
    guard = server_c.find(MODE_GUARD)
    store = server_c.find(STORE_CALL)
    if 0 <= guard < store:
        ok("模式核不过在写入之前（不匹配的记录一个字节都不进汇点）")
    else:
        bad("模式核不过在写入之前", f"{MODE_GUARD} 出现在 {STORE_CALL} 之前",
            f"{guard} vs {store}")


def check_no_writeback(files):
    """第 4 件：链路上不存在会等客户端的点。"""
    server_h = files[SERVER_H]
    server_c = files[SERVER_C]
    source = files[TOOL]

    replies = [name for name in ("send(", "sendto(", "sendmsg(", "sendall(")
               if name in server_c]
    if not replies:
        ok("服务端一个发送调用都没有（只写不回）")
    else:
        bad("服务端一个发送调用都没有", "无 send/sendto/sendmsg/sendall", replies)

    # 只数函数声明（名字后面跟左括号），类型名与结构体标记不算对外接口。
    api = set(re.findall(r"\b(emaster_setpoint_server_\w+)\s*\(", server_h))
    if api == {"emaster_setpoint_server_create", "emaster_setpoint_server_destroy",
               "emaster_setpoint_server_socket_path"}:
        ok("服务端头文件只出建 / 销毁 / 查路径三个符号，没有响应接口")
    else:
        bad("服务端头文件只出建 / 销毁 / 查路径三个符号", "三个", sorted(api))

    if "recv(" not in source and "recvfrom(" not in source and "makefile(" not in source:
        ok("发送端工具从不接收（它无从等待，也无从被回写）")
    else:
        bad("发送端工具从不接收", "无 recv/recvfrom/makefile", "有")

    if "socket.SOCK_SEQPACKET" in source:
        ok("发送端用的也是 SOCK_SEQPACKET（与主站对称）")
    else:
        bad("发送端用的也是 SOCK_SEQPACKET", "socket.SOCK_SEQPACKET", "没找到")


def check(files):
    tool = load_tool(files[TOOL])
    check_geometry(files, tool)
    check_validation(files)
    check_socket(files)
    check_no_writeback(files)


def load(root):
    files = {name: read(root, name) for name in (WIRE_H, WIRE_C, SERVER_H, SERVER_C)}
    files[TOOL] = read(root, TOOL, strip=False)
    return files


def mutate(root):
    """造七个变异副本，逐个跑，确认每个都被抓住。返回 [(名字, 文件集或 None)]。"""
    files = load(root)
    results = []

    def variant(name, path, old, new):
        if old not in files[path]:
            results.append((f"{name}（变异点没找到）", None))
            return
        mutated = dict(files)
        mutated[path] = mutated[path].replace(old, new, 1)
        results.append((name, mutated))

    # 变异 1：轴条目里的目标位置挪两格——C 侧偏移表与 Python 编出的字节当场对不上。
    variant("轴偏移错两位（q 挪到 2）", WIRE_C, "#define AX_OFF_Q      4U", "#define AX_OFF_Q      2U")

    # 变异 2：不校验魔数——别的协议的消息会被当设点记录解。
    variant("解码器不比魔数", WIRE_C,
            "buffer[REC_OFF_MAGIC] != EMASTER_SETPOINT_WIRE_MAGIC_0", "false")

    # 变异 3：不校验长度自洽——record_bytes 说谎时会按轴数硬解出一段错位数据。
    variant("解码器不比长度与轴数自洽", WIRE_C,
            "record_bytes != emaster_setpoint_wire_record_bytes(axis_count)", "false")

    # 变异 4：换成 SOCK_STREAM——消息边界没了，粘包与半条记录全回来。
    variant("换成 SOCK_STREAM", SERVER_C, "socket(AF_UNIX, SOCK_SEQPACKET, 0)",
            "socket(AF_UNIX, SOCK_STREAM, 0)")

    # 变异 5：接收缓冲不留那一个字节——超长消息的尾部被当成合法记录采纳。
    variant("接收缓冲不留 +1", SERVER_C,
            "#define SETPOINT_RX_CAPACITY (EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES + 1U)",
            "#define SETPOINT_RX_CAPACITY (EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES)")

    # 变异 6：给服务端加一条回写——链路上就出现了会等客户端的点。
    variant("服务端回写一条", SERVER_C,
            "            handle_record(server, (size_t)bytes_read);",
            "            (void)send(server->client_fd, server->rx, 1, 0);\n"
            "            handle_record(server, (size_t)bytes_read);")

    # 变异 7：模式核对被短路——模式不匹配的记录照样进输出路径。
    variant("模式核对被短路", SERVER_C, MODE_GUARD, "if (false &&")

    # 变异 8：wire.h 说每轴 20 字节——C 侧偏移表与这一条自相矛盾。
    variant("wire.h 说每轴 20 字节", WIRE_H,
            "#define EMASTER_SETPOINT_WIRE_AXIS_BYTES 24U",
            "#define EMASTER_SETPOINT_WIRE_AXIS_BYTES 20U")

    # 变异 9：Python 那份实现的头缩到 20 字节——两份实现对不上。
    variant("Python 头缩到 20 字节", TOOL, "HEADER_BYTES = 24", "HEADER_BYTES = 20")

    return results


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        global PASSED, FAILED
        variants = mutate(ROOT)
        for name, mutated in variants:
            PASSED = 0
            FAILED = 0
            print(f"--- 变异：{name} ---")
            if mutated is None:
                print("      变异点找不到——这个检查得跟着代码改。")
                return 2
            check(mutated)
            if FAILED:
                print(f"      {FAILED} 项不符（抓住了）")
            else:
                print("      一项都没判红——变异混过去了")
                return 1
        print(f"\n{len(variants)} 个变异全部被抓住（这正是要证明的）")
        return 0

    check(load(ROOT))
    print(f"\n设点通道：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
