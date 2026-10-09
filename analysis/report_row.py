#!/usr/bin/env python3
"""报告的一行表格：把一份报告压成 TSV 的一行，供多轮对照用。

用法：python3 analysis/report_row.py <report.json> [轮次标签]
      python3 analysis/report_row.py --header

为什么单独一件。report_summary.py 出的是给人读的多行文本，适合看一轮；
"连跑 N 轮、看哪几轮像、哪几轮不像"要的是**每轮一行、列对齐**的东西。
把摘要文本再解析回字段是脆的，所以这里直接从报告取字段。

**字段住在哪一层是查过的，不是猜的**——取错地方会安静地退化成「-」，
而「-」在多轮对照里会被读成「这一轮没这项」，不是「我找错地方了」。
当前映射（与 report_summary.py 一致）：
    result                  cycle_count / expected_wkc / actual_wkc / op_reached /
                            all_axes_enabled_reached / motion_completed /
                            wkc_error_count / wkc_no_frame_count
    process_data_delivery   over_budget / frame_gap / frame_interval_max /
                            tail_max_receive_ns / tail_max_receive_ok_ns /
                            tail_max_error_counter_ns / error_counter_read_fail_count /
                            first_mismatch_exchange
    axes[].timing           wkc_mismatch_count / deadline_missed_count（逐轴各一份，取首轴）
    axes[].runtime          max_observed_following_error_counts
    axes[].shutdown_al_entry  state（数「在 OP」用）

缺字段填「-」而不是 0：0 是"量到的是 0"，"-"是"这份报告里没有"，
两者在多轮对照里含义完全不同。
"""
import json
import sys

COLUMNS = (
    "round", "cycles", "wkc_expected", "wkc_actual",
    "wkc_mismatch", "wkc_error", "wkc_no_frame",
    # 判据是"连续"，不是"累计"。策略 config/error_recovery_policies/default.json 里
    # 写的是 wkc_recovery.consecutive_error_threshold=5 与 total_error_threshold=50
    # （后者是 60 秒滑动窗口内的次数，不是整轮累计）。只记累计数会把两种性质不同的
    # 现象看成一件：实测里"散落 2 次"跑完、"连续 5 次"中止，区别只在连续数上。
    #
    # 两类故障各有自己的一套：整帧缺失（actual_wkc<=0）与短帧（帧回来了但计数短）。
    # 连续段是**联合**的（整帧缺失也计入连续），窗口是**分类**的（各推各的），
    # 所以 short_window_max 可能小于 max_consecutive —— 那不是矛盾，是两个口径。
    "wkc_max_consecutive", "wkc_no_frame_max_consecutive",
    "wkc_short_window_max", "wkc_no_frame_window_max",
    "first_mismatch_ex",
    "deadline_missed", "over_budget", "frame_gap", "frame_interval_max",
    "receive_max", "receive_ok_max", "errctr_max", "errctr_fail",
    "op_reached", "all_enabled", "motion_completed",
    "shutdown_in_op", "not_in_op", "following_error_max",
)


def _get(mapping, key, default="-"):
    if not isinstance(mapping, dict):
        return default
    value = mapping.get(key)
    return default if value is None else value


def _bool(value):
    if value is None:
        return "-"
    return "1" if value else "0"


def row_for(path, round_label):
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)

    result = report.get("result") or {}
    delivery = report.get("process_data_delivery") or {}
    axes = report.get("axes") or []
    timing = (axes[0].get("timing") or {}) if axes else {}

    # "停机在OP"只数 state==8。0（没读回来）与 1（INIT）都不是"在 OP"，
    # 三者在报告里长得像，含义差很远——见 monitoring-guide 里那条读法。
    in_op = 0
    not_in_op = []
    for index, axis in enumerate(axes):
        entry = axis.get("shutdown_al_entry") or {}
        state = entry.get("state")
        if state == 8:
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
        _get(timing, "wkc_mismatch_count"),
        _get(result, "wkc_error_count"),
        _get(result, "wkc_no_frame_count"),
        _get(result, "wkc_max_consecutive_errors"),
        _get(result, "wkc_no_frame_max_consecutive_errors"),
        _get(result, "wkc_short_frame_window_max"),
        _get(result, "wkc_no_frame_window_max"),
        _get(delivery, "first_mismatch_exchange"),
        _get(timing, "deadline_missed_count"),
        _get(delivery, "over_budget_cycle_count"),
        _get(delivery, "frame_interval_gap_count"),
        _get(delivery, "frame_interval_max_ns"),
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
    argv = sys.argv[1:]
    want_header = "--header" in argv
    args = [a for a in argv if a != "--header"]
    if want_header:
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
