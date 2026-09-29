#!/usr/bin/env python3
"""由证据目录重建 index.tsv 与各轮的 row.tsv。

用法：python3 scripts/analysis/series_index.py reports/_<系列>

为什么要有这一件。bench_series.sh 在每轮结束时写一行到 index.tsv，用的是**当时那份**
report_row.py——工具本身有 bug 时（2026-09-29 就发生过：两个字段取错了层，列里出现
"-"），已经写进去的行就是错的。而每轮的 report-name.txt 记着那一轮完整报告在归档里
的坐标，所以**整张表随时能从归档重建**。这条重建能力是那件事逼出来的，不是预留的。

重建只读归档报告，不碰总线、不碰硬件。
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
        name_file = os.path.join(directory, "report-name.txt")
        archive = "-"
        if os.path.exists(name_file):
            with open(name_file, encoding="utf-8") as handle:
                archive = handle.read().strip()
        path = os.path.join(ARCHIVE_REL, archive)
        if archive == "-" or not os.path.exists(path):
            missing.append(tag)
            lines.append("\t".join((evidence_dir, "-", "-", "-", "-", "-") + (tag,) +
                                    ("-",) * (len(_row_columns()) - 1)))
            continue
        out = subprocess.run([sys.executable, ROW_TOOL, path, tag],
                             capture_output=True, text=True)
        row = out.stdout.strip()
        with open(os.path.join(directory, "row.tsv"), "w", encoding="utf-8") as handle:
            handle.write(row + "\n")
        lines.append("\t".join((evidence_dir, "-", "-", "-", "-", "-") + tuple(row.split("\t"))))

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
