#!/usr/bin/env python3
"""把一轮的主站日志读成一张异常表（anomalies.tsv）。

用法:
    python3 analysis/anomalies.py <master.log> [-o anomalies.tsv]

为什么要有它。探查的产出是"现象"，而现象必须能跨轮次归并、能定位到坐标，否则每次都要
人眼重读日志，而且不同的人会归并出不同的结果。这张表把日志里已经有的坐标固定下来。

本工具**只读主站日志**，因此：
  · 快，且日志必然存在（报告可能因为磁盘或崩溃而缺失）；
  · 报告侧才有的量（同步裕量、跟随误差）写 unknown —— 这是规格允许的取值，
    比用 0 冒充强。后续可以从报告补齐。

一行的含义是"一类现象的一次聚集"，不是一条日志。同一交换号上多根轴同时报不符，归并成
一行、轴号列成列表：它们几乎总是同一个事件的不同侧面，拆开会让"这一次出了什么事"看不出来。

列的取值来自探索任务书 wholetest/exploration-tasks.md 第 3.4 节。
"""

import argparse
import os
import re
import sys

COLUMNS = ("seq", "kind", "first_exchange", "last_exchange", "count", "axes", "al_state",
           "frame_counts", "sync", "follow_err", "reproducible", "note")

# 主站日志里已经稳定存在的几种行。改主站日志格式时必须同步这里 —— 不匹配不会报错，
# 只会让异常表变空，那比报错更难发现，所以下面有"一条异常都没有"时的显式提示。
RE_MISMATCH = re.compile(
    r"\[WKC\] 首次不符 交换号=(\d+) 轴(\d+): AL state=(\d+), status_code=(0x[0-9A-Fa-f]+)")
RE_CYCLE_FAULT = re.compile(
    r"首次周期异常：交换号=(\d+)，原因=(.*?)，WKC取得=(\S+)，WKC=(\d+)")
RE_SHUTDOWN_ENTRY = re.compile(
    r"\[SHUTDOWN\] 停机入口 轴(\d+): AL state=(\d+), status_code=(0x[0-9A-Fa-f]+)")
RE_SHUTDOWN_SUMMARY = re.compile(r"\[SHUTDOWN\] 停机入口 汇总: (.*)")
RE_BUS_STATE = re.compile(r"总线状态：(.*)")
# 捕获所有"错误："行，但**不**据此判成启动失败：这个前缀也被运行期故障沿用。
# 是否属于启动期，由下面的 START_FAIL_PHRASES 决定；判不出来就记成"错误行"，
# 把原文留给读的人 —— 归错类比漏报更危险，它会把一个运行期问题引到启动路径上去查。
RE_ERROR_LINE = re.compile(r"错误：(.*)")
START_FAIL_PHRASES = ("主站未进入 RUNNING", "套接字未创建", "主站启动失败",
                      "周期控制会话失败", "拒绝发布指纹")
RE_SESSION_FAIL = re.compile(r"CiA 402 周期控制会话失败：(.*)")

# AL 状态码 -> 形态。20 = SAFE-OP（掉出 OP），0 = 未应答（那一站根本没回话），
# 其余按数值原样呈现。8 = OP 是正常值，出现它说明这一行不是故障而是"当时还在 OP"。
AL_KIND = {0: "轴不应答", 20: "轴掉出OP"}


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read().splitlines()
    except OSError as error:
        print("错误：读不了 %s（%s）" % (path, error), file=sys.stderr)
        return None


def format_al(axes_al):
    """把 {轴号: (state, code)} 印成 2(s=20,0x001A),3(s=0,0x0000) 的形式。"""
    if not axes_al:
        return "unknown"
    return ",".join("%d(s=%d,%s)" % (axis, state, code)
                    for axis, (state, code) in sorted(axes_al.items()))


def collect(lines):
    """扫一遍日志，返回行列表（每项是 dict）。"""
    rows = []
    mismatch = {}          # 交换号 -> {轴号: (state, code)}
    shutdown_axes = {}
    shutdown_summary = None
    bus_state = None
    start_errors = []
    session_errors = []
    cycle_faults = []

    for line in lines:
        hit = RE_MISMATCH.search(line)
        if hit:
            exchange = int(hit.group(1))
            mismatch.setdefault(exchange, {})[int(hit.group(2))] = (
                int(hit.group(3)), hit.group(4))
            continue

        hit = RE_CYCLE_FAULT.search(line)
        if hit:
            cycle_faults.append((int(hit.group(1)), hit.group(2), hit.group(4)))
            continue

        hit = RE_SHUTDOWN_ENTRY.search(line)
        if hit:
            shutdown_axes[int(hit.group(1))] = (int(hit.group(2)), hit.group(3))
            continue

        hit = RE_SHUTDOWN_SUMMARY.search(line)
        if hit and shutdown_summary is None:
            shutdown_summary = hit.group(1).strip()
            continue

        hit = RE_BUS_STATE.search(line)
        if hit and bus_state is None:
            bus_state = hit.group(1).strip()
            continue

        hit = RE_SESSION_FAIL.search(line)
        if hit:
            session_errors.append(hit.group(1).strip())
            continue

        hit = RE_ERROR_LINE.search(line)
        if hit:
            start_errors.append(hit.group(1).strip())

    # 匹配上的日志行数。它为零而文件非空，说明日志格式变了而本工具的正则没跟上 ——
    # 那种情况下产出的空表与"这一轮真的没出事"无法区分，必须显式报警。
    matched = len(mismatch) + len(cycle_faults) + len(shutdown_axes)
    if shutdown_summary:
        matched += 1
    if bus_state:
        matched += 1
    matched += len(start_errors) + len(session_errors)

    seq = 0

    def add(kind, first, last, count, axes, note, al=None, frame=None):
        nonlocal seq
        seq += 1
        rows.append({
            "seq": seq, "kind": kind, "first_exchange": first, "last_exchange": last,
            "count": count, "axes": axes or "unknown", "al_state": format_al(al or {}),
            "frame_counts": frame or "unknown", "sync": "unknown", "follow_err": "unknown",
            "reproducible": "unknown", "note": note,
        })

    for exchange in sorted(mismatch):
        axes_al = mismatch[exchange]
        # 形态按这一批里最严重的那一根定：有不应答就记不应答，其次掉出 OP。
        states = {state for state, _ in axes_al.values()}
        if 0 in states:
            kind = "轴不应答"
        elif 20 in states:
            kind = "轴掉出OP"
        else:
            # 全部还是 8（在 OP）—— 说明这一拍是计数变短而轴没掉，属于帧侧问题。
            kind = "计数变短"
        add(kind, exchange, exchange, 1,
            ",".join(str(a) for a in sorted(axes_al)),
            "主站判首次计数不符；同一交换号上 %d 根轴同时报" % len(axes_al),
            al=axes_al)

    for exchange, reason, wkc in cycle_faults:
        add("周期异常", exchange, exchange, 1, "unknown",
            "原因=%s；当时的实得计数=%s" % (reason, wkc))

    if bus_state:
        hit = re.search(r"WKC=期望 (\d+)/最后交换 (\d+)", bus_state)
        frame = "期望=%s 实得=%s" % (hit.group(1), hit.group(2)) if hit else None
        add("总线终态", "unknown", "unknown", 1, "unknown", bus_state, frame=frame)

    if shutdown_summary:
        # 汇总行形如：在OP=5/12；不在OP: 2(s=20),3(s=20)；AL没读回来(state=0): 10 11 12
        in_op = re.search(r"在OP=(\d+)/(\d+)", shutdown_summary)
        not_in_op = ""
        if in_op and in_op.group(1) != in_op.group(2):
            not_in_op = shutdown_summary
            add("停机异常", "unknown", "unknown", 1, "unknown",
                "停机入口时未全部在 OP：%s" % shutdown_summary)
        elif in_op:
            add("停机正常", "unknown", "unknown", 1, "unknown", shutdown_summary)
        del not_in_op

    for message in session_errors:
        # "CiA 402 周期控制会话失败：" 这个前缀**不表示启动期** —— 运行中因计数不符而
        # 中止时，报的是同一句话，只是原因不同。判不出相就别断相：断错会把一个运行期
        # 问题引到启动路径上去查。相由 总线终态 那一行的周期数决定，读的人一眼可见。
        kind = "启动失败" if any(p in message for p in START_FAIL_PHRASES) else "会话失败"
        add(kind, "unknown", "unknown", 1, "unknown", message)
    for message in start_errors:
        # 只有明说了是启动那条路的才算启动失败；其余归"错误行"，原文照录。
        kind = "启动失败" if any(p in message for p in START_FAIL_PHRASES) else "错误行"
        add(kind, "unknown", "unknown", 1, "unknown", message)

    # 停机入口快照里不在 OP 的轴单独列一行：它是"收尾时谁不见了"的直接答案。
    bad = {axis: value for axis, value in shutdown_axes.items() if value[0] != 8}
    if bad:
        add("停机时不在OP", "unknown", "unknown", len(bad),
            ",".join(str(a) for a in sorted(bad)),
            "取自停机入口的逐轴 AL 快照", al=bad)

    return rows, matched


def main():
    parser = argparse.ArgumentParser(description="主站日志 -> 异常表")
    parser.add_argument("master_log")
    parser.add_argument("-o", "--output", help="输出文件；不给则打到标准输出")
    parser.add_argument("--quiet", action="store_true", help="不打印统计")
    args = parser.parse_args()

    lines = read_lines(args.master_log)
    if lines is None:
        return 2
    rows, matched = collect(lines)

    text = "\t".join(COLUMNS) + "\n"
    for row in rows:
        text += "\t".join(str(row[column]) for column in COLUMNS) + "\n"

    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(text)
        if not args.quiet:
            print("已写 %s（%d 行）" % (args.output, len(rows)))
    else:
        sys.stdout.write(text)

    # 一行都没有时要说出来。日志格式改过而正则没跟上，现象就是"表是空的"，
    # 那与"这一轮真的没出事"长得一模一样 —— 这两种必须能分开。
    if matched == 0 and lines and not args.quiet:
        print("警告：日志有 %d 行，但一条已知格式都没匹配上 —— "
              "多半是主站日志的格式变了而本工具的正则没跟上。"
              "此时表里没有异常**不代表这一轮没出事**。" % len(lines), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
