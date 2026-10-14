#!/usr/bin/env python3
""""从命令口停机"是不是真的接上了停机那条路，离线验。

    python3 tests/checks/stop_path_check.py
    python3 tests/checks/stop_path_check.py --mutate   # 证明这检查抓得住"空操作"

为什么值得单独验（P12.1）：命令分支此前写的是**报告字段** `report->stop_requested`，
而安全门读的是**回调** `session->stop_requested`。两个名字像、意思不同，于是从命令口发
stop/shutdown 是一句空话——报告里多一个 true，轴上什么都没发生。这种缺陷不会崩、不会
报错，只会让"我停了它"变成假的，而台架脚本当时还**断言**了这个空操作（"未使主站退出
（已知缺陷）"），等于把缺陷固化成了期望。

硬件上的端到端判据（发一次 stop，主站真的自行退出）只能在台架上做。这里判的是它成立的
**结构前提**，四条：

  1. 两条命令分支（stop / shutdown）各自调用那个抬高安全门输入的入口，且带上来源
     `EMASTER_STOP_SOURCE_COMMAND`；全仓该入口的调用点**恰好两处**——不多开第三条停机路径；
  2. `session_control.c` 里**不许再出现** `report->stop_requested =` 赋值：那是把"停机"
     降级成"记一笔"的老写法；
  3. 那个入口把**命令标志**置真，读取端（accessor）把**命令标志或回调**并起来——两半
     缺一，"汇成一条路"就不成立；
  4. 全仓 `->stop_requested(` 只有**一处**（accessor 里读回调）。两个读点就是两条路，
     迟早有一条忘了改。

变异对照在**内存里的副本**上做（绝不改仓库文件）：把命令分支改回老写法、把 accessor 拆掉
一半、再直读回调一次，三处都必须被判红。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONTROL = "src/bus/session/session_control.c"
SUPERVISOR = "src/bus/session/session_supervisor.c"
INTERNAL = "src/bus/internal/session_internal.h"

REQUEST = "emaster_soem_session_request_stop("
ACCESSOR = "emaster_soem_session_stop_requested("
SOURCE_COMMAND = "EMASTER_STOP_SOURCE_COMMAND"
FLAG = "stop_requested_by_command"

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
    """去注释，只给"数记号"那几条断言用。

    非去不可：不准出现第二个 `->stop_requested(` 的那条规则，就写在
    session_internal.h 的注释里——数原始文本会把规则自己的说明数成一个读点。
    跳过字符串字面量（含转义），免得把 '/*' 之类的字符串当注释起点。
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


def load_sources(root):
    """把 src/ 与 include/ 下的 .c/.h 读成 {相对路径: 文本}。断言只在文本上做。"""
    files = {}
    for directory in ("src", "include"):
        for current, subdirs, names in os.walk(os.path.join(root, directory)):
            subdirs[:] = [d for d in subdirs if d not in ("__pycache__", "external")]
            for name in names:
                if name.endswith((".c", ".h")):
                    full = os.path.join(current, name)
                    with open(full, encoding="utf-8", errors="replace") as handle:
                        files[os.path.relpath(full, root).replace("\\", "/")] = handle.read()
    return files


def count_calls(files, pattern):
    """数**调用点**：带 `void ` 前缀的是声明/定义，不算。注释先去掉。"""
    return sum(len(re.findall(r"(?<!void )" + re.escape(pattern), strip_comments(text)))
               for text in files.values())


def case_body(text, case_label, limit=600):
    """取某个 case 标签到它那个 break 之间的文本；找不到返回 None。"""
    start = text.find(f"case {case_label}:")
    if start < 0:
        return None
    end = text.find("break;", start)
    return text[start:(end + len("break;")) if end > 0 else start + limit]


def check_command_branches(files):
    """1/2：两条命令分支都去抬高安全门输入，且不再有"记一笔"的老写法。"""
    control = files.get(CONTROL)
    if control is None:
        bad(f"{CONTROL} 在", "存在", "读不到")
        return
    for label in ("EMASTER_COMMAND_STOP_MOTION", "EMASTER_COMMAND_SHUTDOWN"):
        body = case_body(control, label)
        if body is None:
            bad(f"命令分支 {label} 在", "存在", "找不到该 case")
        elif REQUEST not in body:
            bad(f"{label} 调用停机入口", REQUEST + f"（带 {SOURCE_COMMAND}）",
                f"这一条分支里没有它：{body.strip()[:160]}")
        elif SOURCE_COMMAND not in body:
            bad(f"{label} 带上停机来源", SOURCE_COMMAND, f"没写来源：{body.strip()[:160]}")
        else:
            ok(f"{label} 抬高的是安全门读的那个输入（带 COMMAND 来源）")

    call_sites = count_calls(files, REQUEST)
    if call_sites == 2:
        ok(f"全仓停机入口恰好 2 处调用点（{REQUEST[:-1]}）")
    else:
        bad("全仓停机入口恰好 2 处调用点", "2",
            f"{call_sites} 处——多一处就是多开了一条停机路径")

    if re.search(r"report->stop_requested\s*=(?!=)", control):
        bad(f"{CONTROL} 里没有把停机降级成记账的赋值", "0 处",
            "出现了 `report->stop_requested =`；报告字段该由安全门那一拍去写，"
            "命令分支写它等于停机动作没发生")
    else:
        ok(f"{CONTROL} 里没有 `report->stop_requested =`（停机不是记账）")


def check_funnel(files):
    """3/4：入口写标志、accessor 并两半、回调只有一个读点。"""
    supervisor = files.get(SUPERVISOR)
    internal = files.get(INTERNAL)
    if supervisor is None or internal is None:
        bad(f"{SUPERVISOR} / {INTERNAL} 在", "存在", "读不到其一")
        return

    request_body = _function_body(supervisor, "void emaster_soem_session_request_stop(")
    if request_body is None:
        bad("找得到停机入口的函数体", "在 supervisor 里", "找不到")
    elif f"session->{FLAG} = true" not in request_body:
        bad("入口把命令标志置真", f"session->{FLAG} = true",
            f"函数体里没有它：{request_body.strip()[:200]}")
    else:
        ok("入口把命令标志置真")

    accessor_body = _function_body(supervisor, "bool emaster_soem_session_stop_requested(")
    if accessor_body is None:
        bad("找得到读取端 accessor 的函数体", "在 supervisor 里", "找不到")
        return
    joined = accessor_body.replace("\n", " ").replace("  ", " ")
    for want, label in ((f"session->{FLAG}", "accessor 读命令标志"),
                        ("session->stop_requested(session->stop_user_data)",
                         "accessor 读回调")):
        if want in joined or want in accessor_body:
            ok(label)
        else:
            bad(label, want, f"函数体里没有它：{accessor_body.strip()[:200]}")

    readers = []
    for path, text in files.items():
        code = strip_comments(text)
        count = code.count("->stop_requested(")
        if count:
            readers.append((path, count))
    total = sum(count for _, count in readers)
    if total == 1:
        ok("全仓只有一处 `->stop_requested(`（回调的唯一读点在 accessor 里）")
    else:
        bad("全仓只有一处 `->stop_requested(`", "1", f"{total} 处：{readers}")

    # 命令标志只该被 supervisor 这个模块碰：别处读到它就是在绕过 accessor。
    strays = [path for path, text in files.items()
              if FLAG in text and path not in (SUPERVISOR, INTERNAL)]
    if strays:
        bad(f"{FLAG} 只出现在 supervisor 与声明它的头文件里", "只有那两处", f"还出现在 {strays}")
    else:
        ok(f"{FLAG} 只出现在 supervisor 与声明它的头文件里（别处只能走 accessor）")

    # 报告字段仍要由安全门那一拍写上，否则报告里就查不到"谁让它停的"。
    if "session->report->stop_requested = true;" in supervisor:
        ok("安全门那一拍仍把停机写进报告（stop_source 才有出处）")
    else:
        bad("安全门那一拍仍把停机写进报告", "session->report->stop_requested = true;",
            "找不到——报告里的停机记录会被一起改没")


def _function_body(text, signature):
    """从签名处取到下一个顶格 `}`，够本检查用（这几个函数都不含内部顶格大括号）。"""
    start = text.find(signature)
    if start < 0:
        return None
    end = text.find("\n}", start)
    return text[start:(end + 2) if end > 0 else len(text)]


def check(files):
    check_command_branches(files)
    check_funnel(files)


def mutate(files):
    """造三个变异副本，逐个跑，确认每个都被抓住。返回 [(名字, 没能抓住?)]。"""
    results = []
    control = files[CONTROL]
    supervisor = files[SUPERVISOR]

    # 变异 1：命令分支改回老写法——只往报告里记一笔。
    old = ('emaster_soem_session_request_stop(\n'
           '                                session, EMASTER_STOP_SOURCE_COMMAND);')
    new = 'session->report->stop_requested = true;  /* 变异：老写法 */'
    mutated = dict(files)
    if control.count(old) >= 1:
        mutated[CONTROL] = control.replace(old, new, 1)
        results.append(("命令分支改回'记一笔'", mutated))
    else:
        results.append(("命令分支改回'记一笔'（变异点没找到）", None))

    # 变异 2：accessor 拆掉一半——只认回调，命令标志等于没人读。
    accessor_line = "    return session->stop_requested_by_command ||\n"
    mutated = dict(files)
    if accessor_line in supervisor:
        mutated[SUPERVISOR] = supervisor.replace(accessor_line, "", 1)
        results.append(("accessor 不再读命令标志", mutated))
    else:
        results.append(("accessor 不再读命令标志（变异点没找到）", None))

    # 变异 3：绕过 accessor，另开一个直读回调的点（两个读点 = 两条路）。
    tunnel = "    stop_requested = emaster_soem_session_stop_requested(session);\n"
    mutated = dict(files)
    if tunnel in supervisor:
        mutated[SUPERVISOR] = supervisor.replace(
            tunnel,
            "    stop_requested = session->stop_requested != NULL &&\n"
            "                     session->stop_requested(session->stop_user_data);  /* 变异 */\n",
            1)
        results.append(("绕过 accessor 直读回调", mutated))
    else:
        results.append(("绕过 accessor 直读回调（变异点没找到）", None))
    return results


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        global PASSED, FAILED
        baseline = load_sources(ROOT)
        # 变异点找不到 = 代码结构变到这个检查跟不上，和"没抓住"一样是坏消息，都得报红。
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
        print("\n三个变异全部被抓住（这正是要证明的）")
        return 0

    files = load_sources(ROOT)
    check(files)
    print(f"\n停机路径的接线：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
