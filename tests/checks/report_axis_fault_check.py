#!/usr/bin/env python3
"""报告里的故障字段：值对不对是 C 用例的事，**这份报告还解析得动吗**是这里的事。

    python3 tests/checks/report_axis_fault_check.py
    python3 tests/checks/report_axis_fault_check.py --mutate   # 少一个闭括号，必须变红

为什么值得单独验（P12.17 的前提）：这一次给报告加了三块逐轴内容（fault /
fault_timeline / final_drive_diagnostic）和运行期 603F，`runtime` 那个对象的收尾
从 `}` 变成 `}}`。少一个括号**不会让任何 C 断言失败**——用例查的是子串，子串照样
在里面——但整份报告会变成解析不了的 JSON，而读报告的工具（extract_report.py、
report_summary.py、GUI）全部当场死掉。现场最难受的正是这个：报告文件在那儿、大小
也正常，谁去读谁报错。

所以分工是刻意的：
  * tests/unit/audit/test_report_fault_fields.c —— 值层面（哪个字段等于几）；
  * 本文件 —— 结构层面：把那份报告喂给 json.loads，解析不过就红。

编译这一步放在这里而不是 CMake，是为了它能在 build 之前跑：CI 的 pre-build 检查
阶段没有二进制，而"报告还能不能解析"这件事不需要整个工程建好。

只在 Linux 上跑（src/audit 的报告库本来就只有 Linux 建，见 src/audit/CMakeLists.txt）。
非 Linux 上打印一行 SKIP 并以 77 退出——**跳过要出声**：静默通过等于把"没验"装成"验过"。
"""

import json
import os
import platform
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST = os.path.join(ROOT, "tests", "unit", "audit", "test_report_fault_fields.c")
SECTIONS = os.path.join(ROOT, "src", "audit", "report_sections.c")
SOURCES = ("src/audit/run_report.c", "src/audit/report_sections.c",
           "src/audit/json_writer.c", "src/cyclic/histogram.c",
           "src/diagnostics/error_class.c", "src/diagnostics/cia402_error.c",
           "src/diagnostics/al_status.c",
           "src/audit/report_error_class.c",
           "src/setpoint/wire.c")

# 变异：把 runtime 对象收尾的两个括号去掉一个。这是"加字段时数错了括号"的真实长相，
# C 照样编得过、C 用例的子串断言照样全过，只有 JSON 会解析不了。
MUTATE_FROM = r'"\"final_diagnostic_successes\":%zu}}",'
MUTATE_TO = r'"\"final_diagnostic_successes\":%zu}",'

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


def build(sections, binary):
    """拿给定的 report_sections.c 建用例（变异时传的是临时副本）。

    要带 -I src/audit：那份 .c 会 include 自己的私有头 report_sections.h。变异副本
    摆在临时目录里，只靠"同目录"是找不到的——少了这个 -I，变异会以"建不起来"的面目
    被判红，看着像抓住了，其实与要验的 JSON 合法性无关（本检查第一版就是这样假绿的）。
    """
    command = ["cc", "-std=c11", "-O0",
               "-I", os.path.join(ROOT, "include"),
               "-I", os.path.join(ROOT, "src", "audit"),
               "-o", binary, TEST]
    command += [os.path.join(ROOT, name) if name != "src/audit/report_sections.c"
                else sections for name in SOURCES]
    proc = subprocess.run(command, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    return proc.returncode == 0, (proc.stdout + proc.stderr)[-600:]


def check(sections, workdir):
    binary = os.path.join(workdir, "test_report_fault_fields")
    built, log = build(sections, binary)
    if not built:
        bad("用例建得起来", "cc 退出码 0", log)
        return
    ok("用例建得起来")

    report = os.path.join(workdir, "report.json")
    proc = subprocess.run([binary, report], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode == 0:
        ok("C 用例的值断言全过（三个轴的字段各就各位）")
    else:
        bad("C 用例的值断言全过", "退出码 0",
            f"退出码 {proc.returncode}；{(proc.stdout + proc.stderr)[-500:]}")

    if not os.path.exists(report):
        bad("报告写出来了", "文件在", f"找不到 {report}")
        return
    try:
        with open(report, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as error:
        bad("报告是一份合法 JSON", "json.loads 通过", f"{type(error).__name__}: {error}")
        return
    ok("报告是一份合法 JSON（json.loads 通过）")

    # 解析得动还不够：三根轴都在，且故障字段确实落在 JSON 的轴对象里——少这一条，
    # 一份只有外壳的空报告也能"解析通过"。
    axes = document.get("axes") or []
    if len(axes) != 3:
        bad("JSON 里三根轴都在", "3", f"{len(axes)}")
    elif not all("fault" in axis and "fault_timeline" in axis for axis in axes):
        bad("每根轴都带 fault / fault_timeline", "三根都有",
            f"{[sorted(k for k in axis if k.startswith('fault')) for axis in axes]}")
    else:
        ok("三根轴都在，且每根都带 fault 与 fault_timeline")


def main():
    global PASSED, FAILED
    if platform.system() != "Linux":
        print(f"SKIP  本机是 {platform.system()}：src/audit 的报告库只在 Linux 上建，"
              f"这里没得验（CI 上会真跑）。")
        return 77

    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        with open(SECTIONS, encoding="utf-8") as handle:
            source = handle.read()
        if MUTATE_FROM not in source:
            print("变异点找不到——report_sections.c 里 runtime 的收尾写法变了，"
                  "这个检查得跟着改。", file=sys.stderr)
            return 2
        # 先跑一遍没变异的。少了这一步，构建坏掉时下面那条"FAILED 非空即算抓住"
        # 会把链接失败当成抓住变异——检查恒绿。这正是一次真实经过：诊断码表加进
        # report_sections.c 后没人更新 SOURCES，本检查红了，变异对照照旧绿。
        with tempfile.TemporaryDirectory() as tmp:
            PASSED = 0
            FAILED = 0
            check(SECTIONS, tmp)
            if FAILED:
                print(f"\n基线就不通过（{FAILED} 项不符）：先修上面这些，"
                      f"这时的对照不说明任何事。", file=sys.stderr)
                return 2

            mutated = os.path.join(tmp, "report_sections_mutated.c")
            with open(mutated, "w", encoding="utf-8") as handle:
                handle.write(source.replace(MUTATE_FROM, MUTATE_TO, 1))
            PASSED = 0
            FAILED = 0
            check(mutated, tmp)
        if FAILED:
            print(f"\n变异被抓住：{FAILED} 项不符（这正是要证明的）")
            return 0
        print("\n变异没被抓住——少一个闭括号还能全身而过，这个检查等于摆设")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        check(SECTIONS, tmp)
    print(f"\n报告故障字段的 JSON 合法性：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
