#!/usr/bin/env python3
"""周期轨迹尾部读出（报告离线判读工具）。

用法：python3 scripts/analysis/trace_tail.py <report.json> [末尾条数]

只打故障前最后 N 条周期现场，一行一条，便于直接看帧距是怎么断的：

    交换号  WKC  帧距(ms)  发送迟到(us)  收包耗时(us)  Sync0余量(us)

帧距 ≈1.000 表示节拍正常；出现 ~2.000 / ~3.000 的整数倍说明整周期被跳过；
迟到持续增长表示周期线程被挡在 CPU 外面。
"""

import json
import sys


def num(row, key):
    value = row.get(key)
    return "?" if value is None else value


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    tail_n = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    with open(sys.argv[1], encoding="utf-8") as handle:
        report = json.load(handle)

    trace = report.get("cycle_trace") or {}
    samples = (trace.get("live") or {}).get("samples") or []
    mismatch = trace.get("mismatch") or {}
    result = report.get("result") or {}

    print("终态 status=%s 首帧异常交换号=%s"
          % (result.get("status_code"), report.get("first_cycle_failure")))

    if mismatch:
        print("异常现场：%s" % json.dumps(mismatch, ensure_ascii=False)[:400])

    if not samples:
        print("轨迹为空（这一轮可能压根没进周期循环）")
        return 0

    print("轨迹共 %d 条，覆盖交换号 %s..%s，下面打最后 %d 条："
          % (len(samples), num(samples[0], "exchange"), num(samples[-1], "exchange"),
             min(tail_n, len(samples))))
    print("  交换号   WKC   帧距ms   迟到us   收包us  邮箱us  错计us  Sync0余量us  旗标")
    for row in samples[-tail_n:]:
        print("  %7s  %4s  %8.3f  %7.1f  %7.1f  %6.1f  %6.1f  %10.1f  0x%X"
              % (num(row, "exchange"), num(row, "wkc"),
                 (num(row, "frame_interval_ns") or 0) / 1e6,
                 (num(row, "send_lateness_ns") or 0) / 1e3,
                 (num(row, "receive_duration_ns") or 0) / 1e3,
                 (num(row, "mailbox_duration_ns") or 0) / 1e3,
                 (num(row, "error_counter_duration_ns") or 0) / 1e3,
                 (num(row, "sync0_margin_ns") or 0) / 1e3,
                 num(row, "flags") or 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
