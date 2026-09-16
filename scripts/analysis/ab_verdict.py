#!/usr/bin/env python3
"""A/B 判据表（报告离线判读工具）。

用法：python3 scripts/analysis/ab_verdict.py <report.json> [report.json ...]

把"运行时帧距缺口"与"停机序言缺口"分开，因为两者都会进 frame_interval 极值，
混在一起会把主次看反：

  轮次 臂 终态 停机gap 运行时最大帧距@交换号 停机段最大帧距 尾部峰值 停机段掉出

判据：frame_interval_max 落在停机首拍之前 = 运行时缺口（放大链）；落在之后 =
停机序言自身的缺口（停机链）。停机首拍交换号取 shutdown_cycles 第一拍的交换号。
"""

import json
import sys


def ms(value):
    return "%.3f" % ((value or 0) / 1e6)


def main():
    print("%-6s %-4s %-5s %-8s %-24s %-13s %-13s %s" % (
        "轮次", "臂", "终态", "停机gap", "运行时最大帧距@交换号", "停机段帧距",
        "尾部峰值ms", "停机段掉出"))
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
        # 运行时 = 极值发生在停机首拍之前（没有停机循环时，正数交换号一律算运行时）
        runtime = None
        shutdown_interval = None
        if first_safe is None or (max_exchange is not None and max_exchange < first_safe):
            runtime = (max_interval, max_exchange)
        else:
            shutdown_interval = (max_interval, max_exchange)

        dropped = [index + 1 for index, axis in enumerate(report.get("axes") or [])
                   if (axis.get("shutdown_al") or {}).get("state") == 20]
        name = path.rsplit("/", 1)[-1].replace("_report.json", "")
        arm = "实验" if name.startswith("e") else "对照"
        cells = (
            name, arm, result.get("status_code"),
            ms(result.get("shutdown_prologue_gap_ns")),
            "%s @%s" % (ms(runtime[0]), runtime[1]) if runtime else "无",
            ms(shutdown_interval[0]) if shutdown_interval else "无",
            ms(delivery.get("tail_uncovered_max_ns")),
            "无" if not dropped else ",".join(map(str, dropped)),
        )
        rows.append(cells)
        print("%-6s %-4s %-5s %-8s %-24s %-13s %-13s %s" % cells)

    for arm in ("实验", "对照"):
        subset = [row for row in rows if row[1] == arm]
        bad = [row for row in subset if row[2] != 0]
        runtime_bad = [row for row in subset if row[4] != "无" and row[2] != 0]
        print("  %s臂：共 %d 轮，非 0 收尾 %d 轮，其中带运行时帧距缺口的 %d 轮"
              % (arm, len(subset), len(bad), len(runtime_bad)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
