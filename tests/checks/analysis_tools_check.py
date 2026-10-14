#!/usr/bin/env python3
"""报告路径不许写死在工具里，离线验。

    python3 tests/checks/analysis_tools_check.py
    python3 tests/checks/analysis_tools_check.py --mutate   # 证明这检查抓得住"读错文件"

为什么值得单独验（P12.30）：仓库里曾有 tools/read_report_summary.py，把报告路径**写死**成
`.../orangepi-dual-with-motion-latest.json`，全文 import sys 却从不使用 sys.argv。后果不是
报错，而是**用"读某份报告"的姿势拿到另一份报告的数字**：给它一个不存在的路径，它照样去开
那个写死的文件（报出来的错是那个路径的），于是"这份报告里没有故障字段"这类结论可以完全
建立在另一份报告上。实际发生过一次——据此差点登记一条假发现，改用直接读 JSON 才纠正过来。
所以这里判两件事：

  1. **静态**：tools/ 与 tools/bench/ 下不许出现"报告文件路径"的字面量（带 reports/ 或以
     /home/orangepi/ 开头且以 .json 收尾，或裸的 `*-latest.json`）。工具要么收参数，
     要么从配置解析；写死的路径在台架机上永远是那一份。

  2. **行为**：正经的那个读取工具（tools/analysis/report_summary.py）给它一个**不存在**
     的路径必须失败，且失败信息里得出现**被要求读的那个**文件名；同时给它一份存在的
     报告必须成功。后半句不是凑数：只判「必须失败」的话，一个永远失败的工具也能通过。

范围只到 tools/ 与 tools/bench/，**不含 docs/**：文档里那些字面量是配置值示例
（`"run_report_path": "runtime/reports/..."`），不是写死的读取路径；一条规则若要把命中项
的大半都豁免掉，那它就不是规则了。

本文件里几个"坏路径"是拆开拼出来的（见 hardcoded_line / _BAD_TOOL）。不拆的话，下半段
那条规则会抓住本文件自己——靠豁免自己通过的检查不再是检查。

变异对照在**副本**上做（绝不改仓库里的文件）：把读取工具改回"写死路径 + 不看参数"，再往
临时目录里放一个同样写死路径的小工具，两处都必须被判红。
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# tools/gen/ 不扫：生成器写出的 run_report_path 是部署默认值，不是"读报告"的工具。
SCAN_DIRS = ("tools",)
SCAN_SKIP = ("tools/gen",)
SCAN_SUFFIXES = (".py", ".sh")
READER = os.path.join(ROOT, "tools", "analysis", "report_summary.py")

# 一份报告文件路径的字面量：目录成分里带 reports/、或以台架机仓库根开头，且以 .json 收尾；
# 外加裸文件名 `*-latest.json`（按本仓库的命名约定，这个形状只可能是报告）。
REPORT_LITERAL = re.compile(
    r"""['"](?:[^'"\n]*(?:reports?/|/home/orangepi/)[^'"\n]*\.json|[A-Za-z0-9_.\-]+-latest\.json)['"]"""
)

# 变异：把"按参数逐份读"换成"读写死的那一份"。这正是被删掉那个工具的写法。
MUTATE_FROM = "    for path in sys.argv[1:]:\n        summarize(path)\n"


def hardcoded_line(tail):
    """造一行"把报告路径写死"的赋值语句。拆开拼：本文件自己也要守下半段那条规则。"""
    return 'path = "' + "/home/" + "orangepi/ethercat-master/runtime/" + "reports/" + tail + '"'


def _mutated_main():
    tail = "orangepi-dual-with-motion-latest" + ".json"
    return ("    paths = [" + '"' + "/home/" + "orangepi/ethercat-master/runtime/reports/"
            + tail + '"' + "]  # 变异：不看参数\n"
            + "    for path in paths:\n        summarize(path)\n")


def _bad_tool():
    return ("#!/usr/bin/env python3\nimport json\n"
            + hardcoded_line("orangepi-quint-30deg-latest" + ".json") + "\n"
            + "with open(path) as handle:\n    json.load(handle)\n")


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


def scan(root):
    """扫出所有把报告文件路径写死的工具。返回 [(相对路径, 行号, 那行)]。"""
    hits = []
    for directory in SCAN_DIRS:
        for current, subdirs, names in os.walk(os.path.join(root, directory)):
            subdirs[:] = [
                d for d in subdirs
                if d != "__pycache__"
                and os.path.relpath(os.path.join(current, d), root) not in SCAN_SKIP
            ]
            for name in names:
                if not name.endswith(SCAN_SUFFIXES):
                    continue
                full = os.path.join(current, name)
                with open(full, encoding="utf-8", errors="replace") as handle:
                    for number, line in enumerate(handle, 1):
                        if REPORT_LITERAL.search(line):
                            hits.append((os.path.relpath(full, root), number, line.strip()))
    return hits


def run_reader(reader, arguments):
    # 显式 utf-8：输出是中文，Windows 上默认按 GBK 解会在这里炸掉，检查的失败长相
    # 就变成"检查自己崩了"，比判错还难查。脚本路径与参数都是 ASCII。
    return subprocess.run([sys.executable, reader] + arguments,
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def probe(reader):
    """行为判：给不存在的一份必须失败并指名，给存在的一份必须成功。"""
    if not os.path.exists(reader):
        bad("读取工具存在", "存在", f"找不到 {reader}")
        return
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "minimal-report.json")
        with open(good, "w", encoding="utf-8") as handle:
            handle.write("{}")
        missing = os.path.join(tmp, "definitely-missing-9f3a.json")

        proc = run_reader(reader, [good])
        if proc.returncode == 0:
            ok("给一份存在的报告，读取工具正常退出（不然「必须失败」那半句谁都能满足）")
        else:
            bad("给一份存在的报告，读取工具正常退出", "退出码 0",
                f"退出码 {proc.returncode}；尾部：{(proc.stdout + proc.stderr)[-300:]}")

        proc = run_reader(reader, [missing])
        said = proc.stdout + proc.stderr
        if proc.returncode == 0:
            bad("给一份不存在的报告，读取工具必须失败", "非 0 退出码",
                "退出码 0——它多半去读了别的文件（P12.30 就是这么发生的）")
        elif os.path.basename(missing) not in said:
            bad("失败信息里指名被要求读的那一份", os.path.basename(missing),
                f"里找不到它；尾部：{said[-300:]}")
        else:
            ok("给一份不存在的报告，读取工具失败并且报出的是那个文件名")


def check(root, reader):
    hits = scan(root)
    if hits:
        bad("没有工具把报告路径写死", "0 处",
            f"{len(hits)} 处：" + "；".join(f"{p}:{n} {line}" for p, n, line in hits[:5]))
    else:
        ok(f"扫过 {'/'.join(SCAN_DIRS)} 下的 .py/.sh，没有报告路径字面量")
    probe(reader)


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        if not os.path.exists(READER):
            print(f"找不到 {READER}", file=sys.stderr)
            return 2
        with open(READER, encoding="utf-8") as handle:
            source = handle.read()
        if MUTATE_FROM not in source:
            print("变异点找不到——读取工具里那段按参数逐份读的写法变了，这个检查得跟着改。",
                  file=sys.stderr)
            return 2
        mutated_source = source.replace(MUTATE_FROM, _mutated_main(), 1)
        with tempfile.TemporaryDirectory() as tmp:
            # 静态那半要一颗"坏工具"的树；行为那半要一份改回写死路径的读取工具。
            tree = os.path.join(tmp, "tree")
            os.makedirs(os.path.join(tree, "tools"))
            os.makedirs(os.path.join(tree, "analysis"))
            with open(os.path.join(tree, "tools", "reader_written_back.py"), "w",
                      encoding="utf-8") as handle:
                handle.write(_bad_tool())
            with open(os.path.join(tree, "analysis", "report_summary.py"), "w",
                      encoding="utf-8") as handle:
                handle.write(mutated_source)
            mutated = os.path.join(tmp, "report_summary_mutated.py")
            with open(mutated, "w", encoding="utf-8") as handle:
                handle.write(mutated_source)
            check(tree, mutated)
        if FAILED:
            print(f"\n变异被抓住：{FAILED} 项不符（这正是要证明的）")
            return 0
        print("\n变异没被抓住——写死路径、不看参数的写法能混过去，这个检查等于摆设")
        return 1

    check(ROOT, os.path.abspath(argv[0]) if argv else READER)
    print(f"\n分析工具的报告路径：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
