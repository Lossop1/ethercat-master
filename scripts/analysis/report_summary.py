#!/usr/bin/env python3
"""报告紧凑摘要（报告离线判读工具）。

用法：python3 scripts/analysis/report_summary.py <report.json> [report.json ...]

只回答三件事：
  1. 这一轮跑了多久（timing.first_exchange / last_exchange，按 1 ms 周期折成秒）；
  2. 故障是什么形态（整帧未回 wkc=-1 / WKC 不符 / 只是迟发），发生在第几次交换，
     那一帧和紧邻几帧的帧距、尾部各段耗时是多少；
  3. 四轴在故障点的 AL 状态与状态码，以及停机时各轴的 AL 收尾状态。
"""
import json
import sys

SEGMENTS = (
    "frame_interval_ns", "send_lateness_ns", "sync0_margin_ns",
    "send_duration_ns", "receive_duration_ns", "mailbox_duration_ns",
    "error_counter_duration_ns", "wkc", "exchange", "flags",
)


def summarize(path):
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)

    print("=" * 78)
    print(path)
    result = report.get("result", {})
    delivery = report.get("process_data_delivery", {})
    axes = report.get("axes") or []
    timing = axes[0].get("timing", {}) if axes else {}
    first = timing.get("first_exchange")
    last = timing.get("last_exchange")
    if isinstance(first, int) and isinstance(last, int):
        print("运行跨度：交换 %d..%d，约 %.1f s" % (first, last, (last - first) / 1000.0))
    print("result.status_code=%s  wkc_error_count=%s  wkc_no_frame_count=%s  "
          "wkc_max_consecutive_errors=%s"
          % (result.get("status_code"), result.get("wkc_error_count"),
             result.get("wkc_no_frame_count"), result.get("wkc_max_consecutive_errors")))
    print("first_mismatch: exchange=%s wkc=%s interval=%s ns  deadline_missed@%s"
          % (delivery.get("first_mismatch_exchange"), delivery.get("first_mismatch_wkc"),
             delivery.get("first_mismatch_frame_interval_ns"),
             delivery.get("first_deadline_missed_exchange")))
    print("尾部最大越界 %s ns @%s；尾部最大 error_counter %s ns；mailbox %s ns"
          % (delivery.get("tail_uncovered_max_ns"), delivery.get("tail_uncovered_max_exchange"),
             delivery.get("tail_max_error_counter_ns"), delivery.get("tail_max_mailbox_ns")))
    print("frame_interval_max=%s ns @%s（停机序言 %s ns）"
          % (delivery.get("frame_interval_max_ns"), delivery.get("frame_interval_max_exchange"),
             result.get("shutdown_prologue_gap_ns")))

    for index, axis in enumerate(axes):
        mismatch = axis.get("first_mismatch") or {}
        print("  axes[%d] %s 首个不符：al_state=%s al_status_code=%s | 停机收尾 %s/%s "
              "| 停机前 %s/%s | deadline_missed=%s"
              % (index, axis.get("axis_id"), mismatch.get("al_state"),
                 mismatch.get("al_status_code"),
                 (axis.get("shutdown_al") or {}).get("state"),
                 (axis.get("shutdown_al") or {}).get("status_code"),
                 (axis.get("shutdown_al_pre_stop") or {}).get("state"),
                 (axis.get("shutdown_al_pre_stop") or {}).get("status_code"),
                 axis.get("timing", {}).get("deadline_missed_count")))

    trace = report.get("cycle_trace", {})
    for name in ("live", "mismatch"):
        holder = trace.get(name) or {}
        samples = holder.get("samples") or []
        if not samples:
            continue
        print("-- trace.%s：%d 条（count=%s）" % (name, len(samples), holder.get("count")))
        # 整帧未回优先看；否则看帧距最大的那几条
        if name == "live":
            tail = samples[-6:]
        else:
            # 环是预触发式：尾部才是故障现场；整帧未回（wkc != 12）单独补进来。
            ordered = sorted(samples, key=lambda s: s.get("exchange", 0))
            picked = {id(sample) for sample in ordered[-10:]}
            picked |= {id(sample) for sample in ordered if sample.get("wkc") != 12}
            tail = [sample for sample in ordered if id(sample) in picked]
        for sample in tail:
            print("   " + "  ".join("%s=%s" % (key, sample.get(key)) for key in SEGMENTS))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        summarize(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
