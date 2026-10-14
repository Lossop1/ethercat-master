#!/usr/bin/env python3
"""停机那两行结论说的是不是真话，离线验。

    python3 tests/checks/shutdown_verdict_check.py
    python3 tests/checks/shutdown_verdict_check.py --mutate   # 证明这检查抓得住改回去

为什么值得单独验。2026-10-13 台架那一轮（11 轴、从零走客户路径）整体通过，唯一一处红在
停机，而它红得自相矛盾：

    shutdown_disable: {attempted: 11, verified: 11, still_enabled: 0, unverified: 0}
    总线状态：... 停用确认=失败 ...
    错误分类：SHUTDOWN（停机阶段失败）

`verified` 那一列数的是"0x6041 读回 bit2=0 的台数"，而 bit2=0 根本不代表已失能（这台驱动器
真使能时也读到 0x1250）。于是同一份报告里"11/11 已验证"和"停用确认=失败"并排出现，读者只能
挑一个信。这一层还有第二个毛病：这台驱动器从不进入 Switch-on-disabled，掩码没填时判据永远
不可能命中，于是每一轮都白等满超时、都报"停机失败"——客户见惯了这个红，真正危险的那次
（仍有轴在运行使能）也就跟着被略过。

硬件上的区分只能在台架上做。这里判的是它成立的三条**结构前提**：

  1. `shutdown_disable` 那三个键说的是**读到了什么**（statusword_bit2_clear 等），
     不是"失能成没成"——键名回到 `verified` 就必须判红；
  2. "未确认"那一档由**末拍状态字**判（每轴解得出来、且都不是 Operation enabled），
     **不看邮箱核对的那几个计数**——换判据、或把它放宽成"读不出也算"都要判红；
  3. 那一档在状态→类别表里落到 `SHUTDOWN_UNCONFIRMED`，且控制台的"停用确认"是三个词
     （通过/未确认/失败），不是两个——回到二值就等于把新档又抹平了。

变异对照在**内存里的副本**上做（绝不改仓库文件）。
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHUTDOWN = "src/bus/session/session_shutdown.c"
SECTIONS = "src/audit/report_sections.c"
CLASSIFY = "src/audit/report_error_class.c"
CLASS_TABLE = "src/diagnostics/error_class.c"
CONSOLE = "apps/master/console.c"
MESSAGES = "config/messages/zh_CN.json"

# C 源码里那个格式串的样子是 \"shutdown_disable\":{\"attempted\":...
BLOCK_MARK = '\\"shutdown_disable\\":{'
NEW_KEYS = ('statusword_bit2_clear', 'statusword_bit2_set', 'statusword_unreadable')
OLD_KEYS = ('verified', 'still_enabled')

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
    """去注释，只留代码与字符串字面量。

    非去不可：这几处说明都写在注释里（注释里就写着"叫 verified 会让两个结论打架"），
    数原始文本会让规则自己的说明把断言喂饱。跳过字符串字面量（含转义），
    因为要测的 JSON 键名正是在格式串里面。
    """
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
    """读一个文件并去注释。读不到返回空串——调用方会把它判成缺记号。"""
    try:
        with open(os.path.join(root, relative), encoding="utf-8", errors="replace") as handle:
            return strip_comments(handle.read())
    except OSError:
        return ""


def function_body(text, signature):
    """从签名处取到下一个顶格 `}`，够本检查用。找不到返回空串。"""
    start = text.find(signature)
    if start < 0:
        return ""
    end = text.find("\n}", start)
    return text[start:(end + 2) if end > 0 else len(text)]


def check(files):
    check_field_names(files)
    check_unconfirmed_tier(files)
    check_verdict_wiring(files)


def check_field_names(files):
    """三个键说的是读到了什么，不是"失能成没成"。"""
    text = files[SECTIONS]
    start = text.find(BLOCK_MARK)
    if start < 0:
        bad("找得到 shutdown_disable 那个格式串", BLOCK_MARK, "没找到")
        return
    block = text[start:start + 400]

    for key in NEW_KEYS:
        if f'\\"{key}\\"' in block:
            ok(f'失能核对的键是 {key}（说的是读到了什么）')
        else:
            bad(f'失能核对的键是 {key}', "格式串里出现这个键", block[:160])
    for key in OLD_KEYS:
        if f'\\"{key}\\"' in block:
            bad(f'失能核对里不再出现 \"{key}\" 这个键',
                "键名不再把'读到了什么'说成'已失能'",
                f'仍然出现 \\"{key}\\"：{block[:160]}')
        else:
            ok(f'失能核对里不再出现 \"{key}\" 这个键')
    if '\\"shutdown_stop_criterion\\"' in block:
        ok("报告记下这一轮用的是哪条判据（紧跟失能核对）")
    else:
        bad("报告记下这一轮用的是哪条判据", '紧跟 shutdown_disable 的 shutdown_stop_criterion',
            block[:160])


def check_unconfirmed_tier(files):
    """"未确认"由末拍状态字判，不靠邮箱核对的那几个计数。"""
    body = function_body(files[SHUTDOWN], "static stop_result_t stop_process_data(")
    if not body:
        bad("找得到 stop_process_data", "函数体", "没找到")
        return

    if "last_all_decoded" in body and "last_any_operation_enabled" in body:
        ok("停机循环逐拍留下末拍的两项事实（解得出来 / 还在运行使能）")
    else:
        bad("停机循环逐拍留下末拍的两项事实", "last_all_decoded 与 last_any_operation_enabled",
            "缺一个")
    if re.search(r"if \(last_all_decoded && !last_any_operation_enabled\)", body):
        ok("判据是'末拍每轴都解得出、且都不是运行使能'")
    else:
        bad("未确认档的判据", "if (last_all_decoded && !last_any_operation_enabled)",
            "这行不在函数体里")
    if "return STOP_RESULT_UNCONFIRMED;" in body:
        ok("未确认单独成一档，不是并到失败里")
    else:
        bad("未确认单独成一档", "return STOP_RESULT_UNCONFIRMED;", "没找到")
    # 邮箱那三个计数是"读到了什么"的记录，不是判据。拿它判 = 把恒绿的检查当结论。
    if "shutdown_statusword_bit2" not in body:
        ok("这一档不看邮箱核对的计数（bit2 不是失能判据）")
    else:
        bad("这一档不看邮箱核对的计数", "函数体里不出现 shutdown_statusword_bit2*",
            "邮箱计数参与了停机判据")
    if "return STOP_RESULT_FAILED;" in body:
        ok("失败那一条还在（读不出、仍有轴在运行使能、交换失败）")
    else:
        bad("失败那一条还在", "return STOP_RESULT_FAILED;", "没找到")


def check_verdict_wiring(files):
    """状态→类别，以及控制台那个三值词。"""
    classify = files[CLASSIFY]
    if ("EMASTER_CONTROL_SESSION_STOP_UNCONFIRMED" in classify and
            "EMASTER_ERROR_CLASS_SHUTDOWN_UNCONFIRMED" in classify):
        ok("停机未确认这一档在状态→类别表里有落点")
    else:
        bad("停机未确认这一档在状态→类别表里有落点",
            "STOP_UNCONFIRMED 映射到 SHUTDOWN_UNCONFIRMED", "缺映射")
    if '"SHUTDOWN_UNCONFIRMED"' in files[CLASS_TABLE]:
        ok("类别表里有稳定 ASCII 键 SHUTDOWN_UNCONFIRMED")
    else:
        bad("类别表里有稳定 ASCII 键 SHUTDOWN_UNCONFIRMED", "表里出现这个键", "没找到")

    console = files[CONSOLE]
    if "stop_confirmation_text(report)" in console:
        ok("汇总行的停用确认走三值函数，不是 result_text(safe_state_reached)")
    else:
        bad("汇总行的停用确认走三值函数", "stop_confirmation_text(report)", "没找到")
    summary = function_body(console, "void emaster_master_console_result(")
    if "stop_confirmation_text(report)" in summary:
        ok("三值函数确实用在那一个槽位上")
    else:
        bad("三值函数确实用在那一个槽位上", "汇总行里调用它", "汇总行里没有")

    try:
        with open(os.path.join(ROOT, MESSAGES), encoding="utf-8-sig") as handle:
            messages = json.load(handle)["messages"]
    except (OSError, ValueError, KeyError):
        messages = {}
    if messages.get("control_session_unconfirmed"):
        ok("消息资源里有'未确认'这个词")
    else:
        bad("消息资源里有'未确认'这个词", "control_session_unconfirmed", "缺键")


def mutate(files):
    """造四个变异副本，逐个跑，确认每个都被抓住。返回 [(名字, 变异后的文件集或 None)]。"""
    results = []

    # 变异 1：键名改回去——那一列读起来又像"已失能"了。
    sections = files[SECTIONS]
    mutated = dict(files)
    if '\\"statusword_bit2_clear\\"' in sections:
        mutated[SECTIONS] = sections.replace(
            '\\"statusword_bit2_clear\\"', '\\"verified\\"', 1)
        results.append(("失能核对的键改回 verified", mutated))
    else:
        results.append(("失能核对的键改回 verified（变异点没找到）", None))

    # 变异 2：未确认改用邮箱计数判——恒绿，等于这一档永远成立。
    shutdown = files[SHUTDOWN]
    guard = "if (last_all_decoded && !last_any_operation_enabled)"
    mutated = dict(files)
    if guard in shutdown:
        mutated[SHUTDOWN] = shutdown.replace(
            guard,
            "if (session->report->shutdown_statusword_bit2_clear_count > 0U)  /* 变异 */",
            1)
        results.append(("未确认改由邮箱计数判", mutated))
    else:
        results.append(("未确认改由邮箱计数判（变异点没找到）", None))

    # 变异 3：类别映射被摘掉——报告里那一档又落回"停机阶段失败"。
    classify = files[CLASSIFY]
    mapping = ("    case EMASTER_CONTROL_SESSION_STOP_UNCONFIRMED:\n"
               "        return EMASTER_ERROR_CLASS_SHUTDOWN_UNCONFIRMED;\n")
    mutated = dict(files)
    if mapping in classify:
        mutated[CLASSIFY] = classify.replace(mapping, "", 1)
        results.append(("停机未确认的类别映射被摘掉", mutated))
    else:
        results.append(("停机未确认的类别映射被摘掉（变异点没找到）", None))

    # 变异 4：停用确认退回二值——新档又被抹平成"失败"。
    console = files[CONSOLE]
    mutated = dict(files)
    if "stop_confirmation_text(report)" in console:
        mutated[CONSOLE] = console.replace(
            "stop_confirmation_text(report)", "result_text(report->safe_state_reached)", 1)
        results.append(("停用确认退回二值", mutated))
    else:
        results.append(("停用确认退回二值（变异点没找到）", None))

    return results


def load(root):
    return {name: read(root, name) for name in
            (SHUTDOWN, SECTIONS, CLASSIFY, CLASS_TABLE, CONSOLE)}


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        global PASSED, FAILED
        baseline = load(ROOT)
        for name, mutated in mutate(baseline):
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
        print("\n四个变异全部被抓住（这正是要证明的）")
        return 0

    check(load(ROOT))
    print(f"\n停机结论的三档与失能字段名：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
