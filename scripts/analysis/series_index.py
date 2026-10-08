#!/usr/bin/env python3
"""由证据目录重建 index.tsv 与各轮的 row.tsv。

用法：python3 scripts/analysis/series_index.py reports/_<系列>

为什么要有这一件。bench_series.sh 在每轮结束时写一行到 index.tsv，用的是**当时那份**
report_row.py——工具本身有 bug 时（2026-09-29 就发生过：两个字段取错了层，列里出现
"-"），已经写进去的行就是错的。而每轮的 report-name.txt 记着那一轮完整报告在归档里
的坐标，所以**整张表随时能从归档重建**。这条重建能力是那件事逼出来的，不是预留的。

重建只读归档报告，不碰总线、不碰硬件。

**重建必须与原索引等价，否则"能从归档重建"这句话是打折的。** 原先那六列前缀
（series/deployment/mode/round_at/gap_s/exit_code）被硬编码成 "-"，理由是它们只存在
于 bench_series.sh 的内存里。现在每轮目录里有 condition.txt 与 exit-code.txt，
这六列是可恢复的 —— 而它们恰恰是跨轮比较时第一步要看的东西（确定两轮的差别在哪）。
本工具从这两份文件取回它们；老系列没有这两份文件时仍然填 "-"，不猜。
"""
import glob
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROW_TOOL = os.path.join(REPO, "scripts", "analysis", "report_row.py")
ARCHIVE_REL = os.path.join("runtime", "reports", "archive")
COLUMNS = ("series", "deployment", "mode", "round_at", "gap_s", "exit_code")


def _row_columns():
    out = subprocess.run([sys.executable, ROW_TOOL, "--header"],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip().split("\t")


def _condition(directory):
    """取回"这一轮施加了什么"。文件不在就返回空字典，由调用方填 "-"。"""
    fields = {}
    path = os.path.join(directory, "condition.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if sep:
                    fields[key.strip()] = value.strip()
    return fields


def _exit_code(directory):
    """真退出码。以单轮脚本自己写下的 exit-code.txt 为准（理由见 bench_series.sh）。"""
    path = os.path.join(directory, "exit-code.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            value = handle.read().strip()
            if value:
                return value
    return "-"


def rebuild(evidence_dir):
    os.chdir(REPO)
    rounds = sorted(d for d in glob.glob(os.path.join(evidence_dir, "r*"))
                    if os.path.isdir(d))
    if not rounds:
        print("错误：%s 下没有 rNN_* 轮次目录" % evidence_dir)
        return 1

    header = "\t".join(COLUMNS + tuple(_row_columns()))
    lines = [header]
    missing = []
    for directory in rounds:
        tag = os.path.basename(directory).split("_", 1)[1]
        condition = _condition(directory)
        prefix = (condition.get("series", evidence_dir),
                  condition.get("deployment", "-"),
                  condition.get("mode", "-"),
                  condition.get("started_at", "-"),
                  condition.get("gap_s", "-"),
                  _exit_code(directory))
        name_file = os.path.join(directory, "report-name.txt")
        archive = "-"
        if os.path.exists(name_file):
            with open(name_file, encoding="utf-8") as handle:
                archive = handle.read().strip()
        path = os.path.join(ARCHIVE_REL, archive)
        if archive == "-" or not os.path.exists(path):
            missing.append(tag)
            lines.append("\t".join(prefix + (tag,) +
                                    ("-",) * (len(_row_columns()) - 1)))
            continue
        out = subprocess.run([sys.executable, ROW_TOOL, path, tag],
                             capture_output=True, text=True)
        row = out.stdout.strip()
        with open(os.path.join(directory, "row.tsv"), "w", encoding="utf-8") as handle:
            handle.write(row + "\n")
        lines.append("\t".join(prefix + tuple(row.split("\t"))))

    index = os.path.join(evidence_dir, "index.tsv")
    with open(index, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("重建 %d 轮 -> %s" % (len(rounds), index))
    if missing:
        print("以下轮次没有归档报告坐标，行里填 -：%s" % ", ".join(missing))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(rebuild(sys.argv[1]))
