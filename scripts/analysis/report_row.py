#!/usr/bin/env python3
"""报告的一行表格：把一份报告压成 TSV 的一行，供多轮对照用。

用法：python3 scripts/analysis/report_row.py <report.json> [轮次标签]

为什么单独一件。report_summary.py 出的是给人读的多行文本，适合看一轮；
"连跑 N 轮、看哪几轮像、哪几轮不像"要的是**每轮一行、列对齐**的东西。
把摘要文本再解析回字段是脆的，所以这里直接从报告取字段。

列（表头由 --header 打印，多轮脚本用它建 index.tsv）：
  轮次 周期数 期望WKC 实际WKC WKC不符 截止期错失 超预算 帧距缺口
  收包max 有效收包max 诊断读max 诊断读失败 OP到达 全轴使能 运动完成
  停机在OP 不在OP的轴 每轴最大跟随误差(逗号分隔)

**缺字段填「-」而不是 0**：0 是"量到的是 0"，"-"是"这份报告里没有"，
两者在多轮对照里含义完全不同。
"""
import json
import sys

COLUMNS = (
    "round", "cycles", "wkc_expected", "wkc_actual", "wkc_mismatch",
    "deadline_missed", "over_budget", "frame_gap",
    "receive_max", "receive_ok_max", "errctr_max", "errctr_fail",
    "op_reached", "all_enabled", "motion_completed",
    "shutdown_in_op", "not_in_op", "following_error_max",
)


def _get(mapping, key, default="-"):
    value = mapping.get(key)
    return default if value is None else value


def _bool(value):
    if value is None:
        return "-"
    return "1" if value else "0"


def row_for(path, round_label):
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)

    result = report.get("result", {})
    delivery = report.get("process_data_delivery", {})
    axes = report.get("axes") or []

    # "停机在OP"只数 state==8。0（没读回来）与 1（INIT）都不是"在 OP"，
    # 而这三者要分开看——见 monitoring-guide 里那条读法。
    in_op = 0
    not_in_op = []
    for index, axis in enumerate(axes):
        entry = (axis.get("shutdown_al_entry") or {})
        if entry.get("state") == 8:
            in_op += 1
        else:
            not_in_op.append("%d(s=%s)" % (index + 1, _get(entry, "state")))

    following = [
        _get((axis.get("runtime") or {}), "max_observed_following_error_counts")
        for axis in axes
    ]

    return [
        round_label,
        _get(result, "cycle_count"),
        _get(result, "expected_wkc"),
        _get(result, "actual_wkc"),
        _get(delivery, "wkc_mismatch_count"),
        _get(delivery, "deadline_missed_count"),
        _get(delivery, "over_budget_cycle_count"),
        _get(delivery, "frame_interval_gap_count"),
        _get(delivery, "tail_max_receive_ns"),
        _get(delivery, "tail_max_receive_ok_ns"),
        _get(delivery, "tail_max_error_counter_ns"),
        _get(delivery, "error_counter_read_fail_count"),
        _bool(result.get("op_reached")),
        _bool(result.get("all_axes_enabled_reached")),
        _bool(result.get("motion_completed")),
        "%d/%d" % (in_op, len(axes)),
        ",".join(not_in_op) if not_in_op else "-",
        ",".join(str(x) for x in following) if following else "-",
    ]


def main():
    args = [a for a in sys.argv[1:] if a != "--header"]
    if "--header" in sys.argv[1:]:
        print("\t".join(COLUMNS))
        if not args:
            return 0
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    label = args[1] if len(args) > 1 else "-"
    print("\t".join(str(x) for x in row_for(path, label)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
