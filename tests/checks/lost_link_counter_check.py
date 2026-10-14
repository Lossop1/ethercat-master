#!/usr/bin/env python3
"""报告能分开"链断过"与"收包超时"吗，离线验。

    python3 tests/checks/lost_link_counter_check.py
    python3 tests/checks/lost_link_counter_check.py --mutate   # 证明这检查抓得住改回去

线型拓扑里断一跳，主站看到的只是"少了几台应答"，与一次收包超时长得一模一样；报告里
另两个错误计数器（rx / forwarded）说的也都是"链还在、帧被收坏"，帮不上。台架上环尾那
个偶发瞬断查不下去，卡的就是这一点。ESC 0x0310 起每端口一个字节的丢失链计数是唯一能分
开两者的读数，所以它必须真的被读到、真的落进报告。

这里判的是四件结构前提（硬件行为在台架上验）：

  1. 周期内那次 FPRD 取的是 0x0300 起 **20 字节**，末尾正好盖住 0x0313；
  2. 每端口的四个字节取自 **偏移 16**（0x0310）——错一位就读到别的计数上去了，
     而读出来的都是小整数，报告里看不错；
  3. 它带着会话第一次读到的基准。这个计数器**上电以来一直累加**，绝对值非零只说明
     "曾经断过"，现场接拔线缆也会加；本轮掉没掉要看差值，所以基准丢了这份读数就
     退回"说不清"。
  4. 报告里有这两个键，且 schema_version 是个正整数（本检查不钉具体版本号，
     理由见下面那段注释）。

同一个块里还读出 forwarded_rx_error_counter：ESC 0x0308 起是**四个单字节**，不是四个
16 位。按 16 位读会把后两个值算成 EPUECNT/PECNT 拼出来的假数——台架干净轮里它们都是
0，所以这个错一直没露头。

变异对照在**内存里的副本**上做（绝不改仓库文件）。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXCHANGE = "src/bus/session/session_exchange.c"
SECTIONS = "src/audit/report_sections.c"

BLOCK = "uint8_t error_block[20];"
READ = "memcpy(session->axes[axis].lost_link_counter, &error_block[16], 4);"
FIRST = "session->axes[axis].lost_link_counter_first_valid = true;"
FRXERR = "memcpy(session->axes[axis].forwarded_rx_error_counter, &error_block[8], 4);"
KEY = '\\"lost_link_counter\\":'
KEY_FIRST = '\\"lost_link_counter_first\\":'

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
    """去注释，只留代码与字符串字面量。注释里写着这几位怎么算，数原始文本会喂饱断言。"""
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


def read(root, relative):
    try:
        with open(os.path.join(root, relative), encoding="utf-8", errors="replace") as handle:
            return strip_comments(handle.read())
    except OSError:
        return ""


def check(files):
    exchange = files[EXCHANGE]

    if BLOCK in exchange:
        ok("FPRD 取 0x0300 起 20 字节（末端盖到 0x0313）")
    else:
        bad("FPRD 取 0x0300 起 20 字节", BLOCK, "没找到")
    if "0x0300U" in exchange:
        ok("读数从 0x0300 起（0x0310 的偏移才算得对）")
    else:
        bad("读数从 0x0300 起", "0x0300U", "没找到")

    if READ in exchange:
        ok("每端口四个字节取自偏移 16（0x0310）")
    else:
        bad("每端口四个字节取自偏移 16（0x0310）", READ, "没找到")

    if FRXERR in exchange:
        ok("FRXERR 按四个单字节读（0x0308，不是四个 16 位）")
    else:
        bad("FRXERR 按四个单字节读", FRXERR, "没找到")

    if FIRST in exchange:
        ok("留下会话第一次读到的基准")
    else:
        bad("留下会话第一次读到的基准", FIRST, "没找到")

    sections = files[SECTIONS]
    for key in (KEY, KEY_FIRST):
        if key in sections:
            ok(f"JSON 里有 {key[2:-2]} 这个键")
        else:
            bad(f"JSON 里有 {key[2:-2]} 这个键", key, "没找到")
    # 版本号不在这里抄一遍：抄了就得两处一起改，而漏改的那次不会红。这里只要求
    # 它是个正整数——本检查钉的是丢失链计数，不是版本号；版本由报告自身的 changelog
    # 与 test_report_fault_fields 管。
    match = re.search(r'\\"schema_version\\":(\d+)', sections)
    if match and int(match.group(1)) >= 1:
        ok(f"schema_version 是个正整数（{match.group(1)}）")
    else:
        bad("schema_version 是个正整数", '\\"schema_version\\":<正整数>', sections[:80])


def mutate(files):
    """造三个变异副本，逐个跑，确认每个都被抓住。返回 [(名字, 变异后的文件集或 None)]。"""
    results = []

    # 变异 1：偏移错一位——读到的是 0x030F 那个空位与前三段丢失链计数，看着还是小整数。
    exchange = files[EXCHANGE]
    mutated = dict(files)
    if "&error_block[16]" in exchange:
        mutated[EXCHANGE] = exchange.replace("&error_block[16]", "&error_block[15]", 1)
        results.append(("丢失链计数偏移错一位", mutated))
    else:
        results.append(("丢失链计数偏移错一位（变异点没找到）", None))

    # 变异 2：块缩到 16 字节——FPRD 根本读不到 0x0310，报告里那四个字节是残留值。
    mutated = dict(files)
    if BLOCK in exchange:
        mutated[EXCHANGE] = exchange.replace(BLOCK, "uint8_t error_block[16];", 1)
        results.append(("FPRD 块缩到 16 字节，够不着 0x0310", mutated))
    else:
        results.append(("FPRD 块缩到 16 字节（变异点没找到）", None))

    # 变异 3：报告里那个键改名——读数还在，但谁也不知道它是哪个计数器。
    sections = files[SECTIONS]
    mutated = dict(files)
    if KEY in sections:
        mutated[SECTIONS] = sections.replace(KEY, '\\"link_drop\\":', 1)
        results.append(("报告里的键改名", mutated))
    else:
        results.append(("报告里的键改名（变异点没找到）", None))

    # 变异 4：基准没了——绝对值非零变成"本轮断过"，把上电以来的累计算进这一轮。
    mutated = dict(files)
    if FIRST in exchange:
        mutated[EXCHANGE] = exchange.replace(FIRST, "session->axes[axis].lost_link_counter_first_valid = false;", 1)
        results.append(("丢掉第一次读到的基准", mutated))
    else:
        results.append(("丢掉第一次读到的基准（变异点没找到）", None))

    # 变异 5：FRXERR 又按 16 位读——后两个值变成 EPUECNT/PECNT 拼出来的假数。
    mutated = dict(files)
    if FRXERR in exchange:
        mutated[EXCHANGE] = exchange.replace(FRXERR, "memcpy(session->axes[axis].forwarded_rx_error_counter, &error_block[8], 8);", 1)
        results.append(("FRXERR 又按 16 位读", mutated))
    else:
        results.append(("FRXERR 又按 16 位读（变异点没找到）", None))

    return results


def load(root):
    return {name: read(root, name) for name in (EXCHANGE, SECTIONS)}


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        global PASSED, FAILED
        for name, mutated in mutate(load(ROOT)):
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
        print("\n五个变异全部被抓住（这正是要证明的）")
        return 0

    check(load(ROOT))
    print(f"\n丢失链计数：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
