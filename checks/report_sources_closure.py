#!/usr/bin/env python3
"""报告库的源文件清单要与它的依赖闭包一致。

为什么需要它。src/audit 的报告库不进 CMake 的常规库（它只在 Linux 上建，见
src/audit/CMakeLists.txt），所以几处检查各自手写一份"要编哪些 .c"：两个
checks/report_*_check.py 的 SOURCES、两个 C 用例头上的 gcc 命令行。加一个模块
（比如诊断码表）而忘了同步这几份清单，结果是**链接失败**。

链接失败本身会喊，但要紧的是它喊成了哪一句：report_axis_fault_check.py 的
--mutate 对照判的是"FAILED 非空"，于是链接失败被当成"抓住了变异"，对照照旧
绿。一次真实的经过：诊断码表接进 report_sections.c 之后，正检查红了、对照绿了，
一红一绿看过去像检查自己在抽风。

所以这里只做一件事：把闭包算出来，逐个文件比对清单。多了不报（多编一个文件
无害），少了是硬失败。

用法：python3 checks/report_sources_closure.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 手写清单的地方。改这几处的组织方式（比如换成从 CMake 读）时，这里要跟着改。
MANIFESTS = [
    "checks/report_axis_fault_check.py",
    "checks/report_archive_check.py",
    "tests/unit/audit/test_report_fault_fields.c",
    "tests/unit/audit/test_report_prune.c",
]

# 闭包的起点：报告库本体。
SEEDS = {"src/audit/run_report.c", "src/audit/report_sections.c",
         "src/audit/json_writer.c", "src/cyclic/histogram.c"}

CALL = re.compile(r"\b(emaster_[a-z0-9_]+)\s*\(")
SOURCE_PATH = re.compile(r"src/[\w/]+\.c")

# 函数定义的写法：行首是返回类型（含 `const char *` 这种指针写法），然后是名字和
# 左括号，行尾没有分号。
# 两个方向的误判代价不对称：把定义误当调用，顶多多算进来一个文件，无害；把调用误当
# 定义，会悄悄吞掉一个缺失的源文件。所以要求行首有返回类型，并且那个类型里不许出现
# 控制关键字或等号——`        return emaster_foo(`（跨行调用的第一行）就是靠这条挡住的。
# 返回类型单独占一行的定义会被漏掉，那不要紧：它的定义就在本文件里，本来就在集合里。
DEFINITION = re.compile(
    r"^\s*([A-Za-z_][\w \t]*[\w \t\*]*)\s*(emaster_[a-z0-9_]+)\s*\([^;]*$", re.MULTILINE)
NOT_A_TYPE = re.compile(
    r"\b(return|if|while|for|switch|case|else|do|goto|sizeof|typedef)\b|=")


def definitions_in(text):
    found = set()
    for match in DEFINITION.finditer(text):
        if NOT_A_TYPE.search(match.group(1)):
            continue
        found.add(match.group(2))
    return found

DEFINED = {}


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def read_c(relative):
    return strip_comments((ROOT / relative).read_text(encoding="utf-8", errors="replace"))


def index_definitions():
    """全仓扫一遍：每个 emaster_ 符号定义在哪个 .c 里。"""
    for path in (ROOT / "src").rglob("*.c"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for symbol in definitions_in(text):
            DEFINED.setdefault(symbol, set()).add(path.relative_to(ROOT).as_posix())


def closure(seeds, unresolved):
    """从 seeds 出发，把被调用但不在集合里的符号所在的 .c 逐个补进来。"""
    current = set(seeds)
    while True:
        called, defined_here = set(), set()
        for source in sorted(current):
            text = read_c(source)
            called |= set(CALL.findall(text))
            defined_here |= definitions_in(text)
        added = set()
        for symbol in sorted(called - defined_here):
            where = DEFINED.get(symbol, set())
            fresh = where - current
            if fresh:
                added |= fresh
            elif not where:
                unresolved.add(symbol)
        if not added:
            return current
        current |= added


def declared(relative):
    text = (ROOT / relative).read_text(encoding="utf-8", errors="replace")
    return {path for path in SOURCE_PATH.findall(text) if (ROOT / path).is_file()}


def main():
    index_definitions()
    unresolved = set()
    needed = closure(SEEDS, unresolved)

    if unresolved:
        # 静态内联或宏：它们在头文件里，不是要补的 .c。说出来，别当无事发生。
        print("闭包里有符号在 src/ 下找不到定义（按静态内联或宏处理）：")
        for symbol in sorted(unresolved):
            print(f"    {symbol}")

    failed = False
    for relative in MANIFESTS:
        have = declared(relative)
        missing = sorted(needed - have)
        if missing:
            failed = True
            print(f"{relative} 的源文件清单少了 {len(missing)} 个：")
            for path in missing:
                print(f"    {path}")
    if failed:
        print("\n把缺的补进上面每个文件的清单。这份清单是手写的，闭包是算出来的。")
        return 1

    print(f"报告库源文件清单与闭包一致：{len(needed)} 个文件，"
          f"{len(MANIFESTS)} 处清单都对得上。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
