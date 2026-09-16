#!/usr/bin/env python3
"""序言融入周期的 A/B 判据表（报告离线判读工具）。

用法：python3 scripts/analysis/inline_verdict.py /tmp/i*_report.json /tmp/c*_report.json

每轮一行：

  轮次 臂 终态 inline 序言帧数 序言跨度ms 停机gapms 全轮最大帧距@交换号 掉出轴

判读要点：

  · 物理量只有一个——**全轮最大帧距**（process_data_delivery.frame_interval_max_ns）。
    驱动器判的是"两帧隔了多久"，停机序言缺口与运行时放大链都会在这里露出来，
    只是发生的位置不同（靠交换号区分：落在停机首拍之前是运行时，之后是停机段）。
  · 对照臂若出现 2～4 ms 的全轮最大帧距，而实验臂一路压在 1.0x ms，
    则"序言期间不停供"成立；
  · 实验臂 report 里 inline_mode 必须为 true、inline_drain_cycles > 0，
    否则说明开关根本没走到，这批不能算证据；
  · 掉出轴一栏取自停机结束时的 AL 状态（20 = SAFE-OP，即掉出 OP）。
"""

import json
import sys


def ms(value):
    return "%.3f" % ((value or 0) / 1e6)


def main():
    print("%-6s %-4s %-5s %-7s %-9s %-10s %-10s %-22s %s" % (
        "轮次", "臂", "终态", "inline", "序言帧数", "序言跨度ms", "停机gapms",
        "全轮最大帧距@交换号", "掉出轴"))
    rows = []
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as handle:
            report = json.load(handle)
        result = report.get("result") or {}
        delivery = report.get("process_data_delivery") or {}
        prologue = report.get("shutdown_prologue") or {}
        cycles = report.get("shutdown_cycles") or {}
        samples = cycles.get("samples") or []
        first_safe = samples[0].get("exchange") if samples else None

        max_interval = delivery.get("frame_interval_max_ns") or 0
        max_exchange = delivery.get("frame_interval_max_exchange")
        where = "停机段" if (first_safe is not None and max_exchange is not None
                            and max_exchange >= first_safe) else "运行时"

        dropped = [index + 1 for index, axis in enumerate(report.get("axes") or [])
                   if (axis.get("shutdown_al") or {}).get("state") == 20]
        name = path.rsplit("/", 1)[-1].replace("_report.json", "")
        arm = "实验" if name.startswith("i") else "对照"
        cells = (
            name, arm, result.get("status_code"),
            "是" if prologue.get("inline_mode") else "否",
            prologue.get("inline_drain_cycles") or 0,
            ms(prologue.get("inline_drain_ns")),
            ms(result.get("shutdown_prologue_gap_ns")),
            "%s (%s) @%s" % (ms(max_interval), where, max_exchange),
            "无" if not dropped else ",".join(map(str, dropped)),
        )
        rows.append(cells)
        print("%-6s %-4s %-5s %-7s %-9s %-10s %-10s %-22s %s" % cells)

    for arm in ("实验", "对照"):
        subset = [row for row in rows if row[1] == arm]
        if not subset:
            continue
        bad = [row for row in subset if row[2] != 0]
        not_run = [row for row in subset if row[3] == "否"] if arm == "实验" else []
        print("  %s臂：共 %d 轮，非 0 收尾 %d 轮，掉出轴非空 %d 轮"
              % (arm, len(subset), len(bad),
                 len([row for row in subset if row[8] != "无"])))
        if not_run:
            print("    ！！实验臂有 %d 轮 inline_mode 为假，开关没走到：%s"
                  % (len(not_run), ",".join(row[0] for row in not_run)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
