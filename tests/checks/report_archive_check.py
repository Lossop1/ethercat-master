#!/usr/bin/env python3
"""归档削旧：删了谁、删的时候出没出声，离线验。

    python3 tests/checks/report_archive_check.py
    python3 tests/checks/report_archive_check.py --mutate   # 两处对照，见下

为什么值得单独验（P12.29）：`prune_archives` 此前只调 unlink、**一个字都不打**。
于是"报告不见了"和"这一轮根本没跑"在现场长得一模一样——P11.5 的 r5–r10 就是这么
丢的，十轮 A/B 的底稿被后面几轮顶掉，只剩文档里手抄的一份计数，而"哪一轮丢过帧"
恰恰要靠回头比对才能回答。

**删除留声是子进程 stderr 的事**，所以判在 Python 里：在 C 里判得重定向 fd 再恢复，
比要验的东西本身更容易写歪。C 那边（tests/unit/audit/test_report_prune.c）管的是
数量与对象——削到几份、留的是最旧还是最新、别的前缀动没动、keep=0 时删不删。

变异对照两个，各盯一半：
  A. 把那行删除日志拿掉 → 本文件必须红（判的是 stderr）；
  B. 把两处 `keep == 0` 护栏一起拆掉 → C 用例必须红（P12.32：0 会把同前缀的每一份
     都删光）。只拆 prune 那一道是**等价变异**，单看 C 用例看不出差别，原因写在
     用例的头部注释里。

只在 Linux 上跑（src/audit 的报告库本来就只有 Linux 建）。非 Linux 上打印 SKIP 并以
77 退出——跳过要出声，静默通过等于把"没验"装成"验过"。
"""

import os
import platform
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST = os.path.join(ROOT, "tests", "unit", "audit", "test_report_prune.c")
RUN_REPORT = os.path.join(ROOT, "src", "audit", "run_report.c")
MODEL = os.path.join(ROOT, "tools", "gen", "runtime_config_model.py")
# report_sections.c 要调诊断码表（类别键、类别文本、CiA 402 码解释、AL 码判同步），
# 少一个源文件就是链接失败——而失败长成"用例建得起来"那一条不对，看着像检查本身坏了。
# 这份清单是 tests/checks/report_sources_closure.py 算出来的闭包；加模块后重算一次，别靠撞链接错误。
SOURCES = ("src/audit/run_report.c", "src/audit/report_sections.c",
           "src/audit/json_writer.c", "src/cyclic/histogram.c",
           "src/diagnostics/error_class.c", "src/diagnostics/cia402_error.c",
           "src/diagnostics/al_status.c",
           "src/audit/report_error_class.c",
           "src/setpoint/wire.c")

# 用例里的形状：25 份旧的 + 刚发布那一份，保留 20 份 ⇒ 削掉 6 份，正是最旧的那 6 份。
DELETED = 6
KEEP = 20
VICTIMS = [f"bench-latest-20250101T0000{index:02d}Z.json" for index in range(DELETED)]
SURVIVOR = f"bench-latest-20250101T0000{DELETED:02d}Z.json"

# 变异 A：删了不出声（把 fprintf 整个拿掉，保留 victim 被用到，免得变成未使用变量告警）。
MUTATE_LOG_FROM = ('            fprintf(stderr,\n'
                   '                    "归档削旧：%s 被删除（前缀 %s 下原有 %zu 份，'
                   '保留 %lu 份）\\n",\n'
                   '                    victim, prefix, count, (unsigned long)keep);')
MUTATE_LOG_TO = '            (void)victim;  /* 变异：删了不出声 */'

# 变异 B：两道 keep == 0 护栏一起拆（单拆一道是等价变异，见用例头部注释）。
MUTATE_GUARD_PRUNE_FROM = ("    if (keep == 0U)\n    {\n        return;\n    }\n\n"
                           "    dir = opendir(directory);")
MUTATE_GUARD_PRUNE_TO = "    dir = opendir(directory);"
MUTATE_GUARD_ARCHIVE_FROM = ("    if (keep == 0U)\n    {\n        return;\n    }\n"
                             "    if (!archive_directory_for(path, directory, "
                             "sizeof(directory)) ||")
MUTATE_GUARD_ARCHIVE_TO = ("    if (!archive_directory_for(path, directory, "
                           "sizeof(directory)) ||")

# 默认保留份数不许悄悄退回小值：这个默认值才是实际生效的那个，只改部署等于没改
# （P12.29 就是这么来的：默认 20、9 份部署无一覆盖）。台架一次 A/B 十几轮，20 份
# 连一轮都留不住。允许部署显式选小值甚至 0，但**默认**得留得住。
KEEP_FLOOR = 100

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


def build(run_report_source, binary):
    command = ["cc", "-std=c11", "-O0",
               "-I", os.path.join(ROOT, "include"),
               "-I", os.path.join(ROOT, "src", "audit"),
               "-o", binary, TEST]
    command += [run_report_source if name == "src/audit/run_report.c"
                else os.path.join(ROOT, name) for name in SOURCES]
    proc = subprocess.run(command, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    return proc.returncode == 0, (proc.stdout + proc.stderr)[-600:]


def check_keep_floor():
    with open(MODEL, encoding="utf-8") as handle:
        source = handle.read()
    match = re.search(r"^DEFAULT_REPORT_ARCHIVE_KEEP\s*=\s*(\d+)", source, re.MULTILINE)
    if match is None:
        bad("找得到默认保留份数", "DEFAULT_REPORT_ARCHIVE_KEEP = <数>",
            "runtime_config_model.py 里找不到它")
        return
    value = int(match.group(1))
    if value >= KEEP_FLOOR:
        ok(f"默认保留份数是 {value}（≥{KEEP_FLOOR}）")
    else:
        bad("默认保留份数够一次 A/B 用", f"≥ {KEEP_FLOOR}",
            f"{value}——默认值才是实际生效的，调小就等于把 P12.29 放回来")


def check(run_report_source, workdir):
    check_keep_floor()

    binary = os.path.join(workdir, "test_report_prune")
    built, log = build(run_report_source, binary)
    if not built:
        bad("用例建得起来", "cc 退出码 0", log)
        return
    ok("用例建得起来")

    proc = subprocess.run([binary], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode == 0:
        ok("C 用例的删旧断言全过（份数、对象、别的前缀、keep=0）")
    else:
        bad("C 用例的删旧断言全过", "退出码 0",
            f"退出码 {proc.returncode}；{(proc.stdout + proc.stderr)[-500:]}")

    log_lines = [line for line in proc.stderr.splitlines() if line.startswith("归档削旧：")]
    failures = [line for line in proc.stderr.splitlines() if "归档削旧失败" in line]
    if failures:
        bad("削旧过程中没有删失败的", "0 行", f"{failures[:3]}")
    elif len(log_lines) != DELETED:
        bad("每删一份都留一行日志", f"{DELETED} 行",
            f"{len(log_lines)} 行：{log_lines[:8]}")
    else:
        ok(f"每删一份都留一行日志（{DELETED} 行，一条不多一条不少）")

    named = [os.path.basename(re.search(r"归档削旧：(\S+) 被删除", line).group(1))
             for line in log_lines
             if re.search(r"归档削旧：(\S+) 被删除", line)]
    if sorted(named) == sorted(VICTIMS):
        ok("日志点名的正是被删的那几份（最旧的 6 份）")
    else:
        bad("日志点名的正是被删的那几份", sorted(VICTIMS), sorted(named))

    if SURVIVOR in named:
        bad("留下来了的那一份不该出现在删除日志里", f"不含 {SURVIVOR}", "它在被删名单里")
    else:
        ok(f"留下来了的那一份（{SURVIVOR}）不在被删名单里")

    without_keep = [line for line in log_lines if f"保留 {KEEP} 份" not in line]
    if without_keep:
        bad("日志里说清了按几份在留", f"每行都带「保留 {KEEP} 份」",
            f"{len(without_keep)} 行没带：{without_keep[:2]}")
    else:
        ok(f"日志里说清了按几份在留（保留 {KEEP} 份）")


def main():
    if platform.system() != "Linux":
        print(f"SKIP  本机是 {platform.system()}：src/audit 的报告库只在 Linux 上建，"
              f"这里没得验（CI 上会真跑）。")
        return 77

    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        with open(RUN_REPORT, encoding="utf-8") as handle:
            source = handle.read()
        for anchor in (MUTATE_LOG_FROM, MUTATE_GUARD_PRUNE_FROM, MUTATE_GUARD_ARCHIVE_FROM):
            if anchor not in source:
                print("变异点找不到——run_report.c 的形状变了，这个检查得跟着改。",
                      file=sys.stderr)
                return 2
        mutants = {
            "删了不出声": source.replace(MUTATE_LOG_FROM, MUTATE_LOG_TO, 1),
            "两道 keep=0 护栏都拆掉": source
            .replace(MUTATE_GUARD_PRUNE_FROM, MUTATE_GUARD_PRUNE_TO, 1)
            .replace(MUTATE_GUARD_ARCHIVE_FROM, MUTATE_GUARD_ARCHIVE_TO, 1),
        }
        global PASSED, FAILED
        for name, text in mutants.items():
            PASSED = 0
            FAILED = 0
            print(f"--- 变异：{name} ---")
            with tempfile.TemporaryDirectory() as tmp:
                mutated = os.path.join(tmp, "run_report_mutated.c")
                with open(mutated, "w", encoding="utf-8") as handle:
                    handle.write(text)
                check(mutated, tmp)
            if FAILED:
                print(f"      {FAILED} 项不符（抓住了）")
            else:
                print("      一项都没判红——变异混过去了")
                return 1
        print("\n两个变异都被抓住（这正是要证明的）")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        check(RUN_REPORT, tmp)
    print(f"\n归档削旧与删除留声：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
