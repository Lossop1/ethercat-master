#!/usr/bin/env python3
"""把报告里的异常窗口读成一张逐拍的表。

用法:
    python3 scripts/analysis/trace_window.py <报告.json> [--span N] [--step N]

报告里的 cycle_trace.mismatch 是一个「前 200 拍 + 异常那一拍 + 后 200 拍」的窗口。
它存在的理由是：汇总列（最大收包时长、不符次数、连续段长度）只能说明"坏了"，
说明不了"怎么坏的"——2026-10-08 实测到两轮因完全不同的原因失败，而在汇总列里
各字段几乎一样（receive_max 199225 对 274193，区间重叠）。

**trigger_index 必须按它读，不能按"中间那一拍"猜。** 窗口收满时它是 pre_capacity；
会话在"之后"那段走完之前就结束（异常后很快中止）时它更小，此时窗口尾部并没有
200 拍"之后"的数据。

列的含义（空值写 . 而不是 0：0 在多数列上是合法值）：
    idx        窗口内下标
    exch       交换号；与上一行的差 > 1 表示中间跳了拍
    wkc        实得工作计数
    flg        标志位：M=计数不符  E=错误计数器读失败
    recv/send  收包段 / 发包段耗时（微秒）
    mbox/ec    邮箱推进 / 错误计数器读耗时（微秒）
    lat        send_lateness（微秒，正=发晚了）
    marg       sync0 裕量（微秒，负=输出对驱动器已经迟到）
    fgap       本拍与上一拍的实际帧距（微秒）

退出码：0 = 正常；1 = 报告里没有窗口或读不了；2 = 用法错。
"""

import argparse
import json
import sys

TRIGGER_MARK = "  <== 首个不符"


def us(value):
    """纳秒转微秒；缺值写点。"""
    if value is None:
        return "."
    return "%.0f" % (float(value) / 1000.0)


def flags_text(flags):
    text = ""
    if flags & 0x0001:
        text += "M"
    if flags & 0x0002:
        text += "E"
    return text or "."


def main():
    parser = argparse.ArgumentParser(description="读报告里的异常窗口")
    parser.add_argument("report")
    parser.add_argument("--span", type=int, default=12,
                        help="异常前后各印多少拍（默认 12；0=全印）")
    parser.add_argument("--step", type=int, default=1, help="抽样步长，用于全印时压缩输出")
    args = parser.parse_args()

    try:
        with open(args.report, encoding="utf-8") as handle:
            report = json.load(handle)
    except (OSError, ValueError) as error:
        print("错误：读不了 %s（%s）" % (args.report, error), file=sys.stderr)
        return 1

    trace = report.get("cycle_trace") or {}
    window = trace.get("mismatch") or {}
    samples = window.get("samples") or []
    if not window.get("present") or not samples:
        print("这份报告里没有异常窗口（mismatch.present=%s，samples=%d）。"
              % (window.get("present"), len(samples)))
        return 1

    trigger = window.get("trigger_index")
    if trigger is None:
        # 没有 trigger_index 的报告是加这个字段之前的格式。此时只能按"中间那一拍"猜，
        # 而那正是本工具要求不要做的事 —— 明说，不静默降级。
        print("这份报告的窗口没有 trigger_index，无法确定哪一拍是异常（旧格式）。",
              file=sys.stderr)
        return 1

    print("窗口：%d 拍（前 %s + 异常那一拍 + 后 %s），trigger_index=%d，异常交换号=%s，WKC=%s"
          % (len(samples), window.get("pre_capacity", "?"), window.get("post_capacity", "?"),
             trigger, window.get("exchange"), window.get("wkc")))

    if args.span:
        lo = max(0, trigger - args.span)
        hi = min(len(samples), trigger + args.span + 1)
        indexes = list(range(lo, hi))
    else:
        indexes = list(range(0, len(samples), max(1, args.step)))

    header = ("idx", "exch", "wkc", "flg", "recv", "send", "mbox", "ec", "lat", "marg", "fgap")
    print("%-5s %-9s %-5s %-4s %-7s %-7s %-7s %-6s %-7s %-7s %-7s"
          % header)

    previous = None
    for index in indexes:
        sample = samples[index]
        exchange = sample.get("exchange")
        # 交换号跳号说明中间有拍没进环（或没发出去）。汇总列看不到这个。
        gap = ""
        if previous is not None and exchange is not None and exchange - previous > 1:
            gap = "  <<< 跳 %d 拍" % (exchange - previous - 1)
        print("%-5d %-9s %-5s %-4s %-7s %-7s %-7s %-6s %-7s %-7s %-7s%s%s"
              % (index, exchange, sample.get("wkc", "."), flags_text(sample.get("flags", 0)),
                 us(sample.get("receive_duration_ns")), us(sample.get("send_duration_ns")),
                 us(sample.get("mailbox_duration_ns")), us(sample.get("error_counter_duration_ns")),
                 us(sample.get("send_lateness_ns")), us(sample.get("sync0_margin_ns")),
                 us(sample.get("frame_interval_ns")),
                 TRIGGER_MARK if index == trigger else "", gap))
        previous = exchange
    return 0


if __name__ == "__main__":
    sys.exit(main())
