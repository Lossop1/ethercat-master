#!/usr/bin/env python3
"""把审计报告里"发出去的"和"实际发生的"并到同一条时间线上。

审计报告按 (字段, 值变化) 存记录，每条带 first_exchange/last_exchange/sample_count。
报告里同时有 RxPDO（direction=write：主站每拍写进帧里的控制字/模式/目标）和
TxPDO（direction=read：驱动器每拍回报的状态字/实际位置/实际速度/实际力矩）。
两边用的是同一个 exchange 编号，所以能逐拍对齐——这就是"发了什么"和"发生了什么"
本该在的那一页。

默认只打**变化点**（任何被跟踪字段一变就出一行），不是每一拍：一拍一行跑几千行
没人看，变化点才是能被读的因果链。要看连续段用 --step。

用法:
  python3 tools/bench/report_timeline.py <报告.json> [--axis 1] [--step N]
      [--from E] [--to E] [--phase cyclic_operation]
"""

import argparse
import json
import sys

# 逐拍会被记进审计的字段。读的（驱动器报的）与写的（主站发的）分开列，
# 打出来时左右并排，一眼能看上谁先动。
READ_OBJECTS = ["0x6041:00", "0x6061:00", "0x6064:00", "0x606C:00", "0x6077:00"]
WRITE_OBJECTS = ["0x6040:00", "0x6060:00", "0x607A:00", "0x60FF:00"]

# 打印短名：坐标头要塞得下。
SHORT = {
    "0x6040:00": "6040",
    "0x6060:00": "6060",
    "0x607A:00": "607A",
    "0x60FF:00": "60FF",
    "0x6041:00": "6041",
    "0x6061:00": "6061",
    "0x6064:00": "6064",
    "0x606C:00": "606C",
    "0x6077:00": "6077",
}

# 只在真变的时候才有意义的字段：状态字、控制字、模式、各处目标。
DISCRETE_OBJECTS = {"0x6040:00", "0x6041:00", "0x6060:00", "0x6061:00", "0x607A:00", "0x60FF:00"}

# 都是十六进制的字段，打印时带 0x。
HEX_OBJECTS = {"0x6040:00", "0x6041:00", "0x603F:00"}


def load_axis(report, axis_position, phase):
    """把所有 (字段, 方向) 的值变化记录按 exchange 展开成 {字段: {exchange: 值}}。

    报告里同方向同字段可以有多条记录（值变一次存一条），展开区间就是逐拍值。
    """
    series = {}
    for access in report["accesses"]:
        if access["phase"] != phase:
            continue
        if access["slave_position"] != axis_position:
            continue
        key = access["object"]
        if key not in READ_OBJECTS and key not in WRITE_OBJECTS:
            continue
        table = series.setdefault(key, {"direction": access["direction"], "values": {}})
        first = access["first_exchange"]
        last = access["last_exchange"]
        for exchange in range(first, last + 1):
            table["values"][exchange] = access["value_dec"]
    return series


def format_value(key, value):
    if value is None:
        return "-"
    if key in HEX_OBJECTS:
        return f"0x{value:04X}"
    return str(value)


def render(series, exchanges, header):
    columns = [key for key in READ_OBJECTS + WRITE_OBJECTS if key in series]
    widths = {}
    for key in columns:
        label = SHORT[key]
        body = max((len(format_value(key, series[key]["values"].get(e))) for e in exchanges),
                   default=1)
        widths[key] = max(len(label), body)
    line = f"{'拍':>6}  " + "  ".join(
        f"{'发' if k in WRITE_OBJECTS else '报'}{SHORT[k]:>{widths[k] + 1}}"
        for k in columns)
    print(header)
    print(line)
    for exchange in exchanges:
        cells = []
        for key in columns:
            value = format_value(key, series[key]["values"].get(exchange))
            cells.append(f"{value:>{widths[key] + 2}}")
        print(f"{exchange:>6}  " + "  ".join(cells))


def main(argv=None):
    parser = argparse.ArgumentParser(description="审计报告：发送与结果逐拍对齐")
    parser.add_argument("report", help="审计报告 JSON 路径")
    parser.add_argument("--axis", type=int, default=1, help="从站号（默认 1）")
    parser.add_argument("--phase", default="cyclic_operation", help="只取这个阶段（默认 cyclic_operation）")
    parser.add_argument("--from", dest="first", type=int, default=None, help="起始 exchange")
    parser.add_argument("--to", dest="last", type=int, default=None, help="结束 exchange")
    parser.add_argument("--step", type=int, default=None,
                        help="每一拍都打（N>1 则每 N 拍一行）；不给则只打变化点")
    args = parser.parse_args(argv)

    with open(args.report, encoding="utf-8") as handle:
        report = json.load(handle)
    series = load_axis(report, args.axis, args.phase)
    if not series:
        raise SystemExit(f"报告里没有轴 {args.axis} 的 {args.phase} 记录")

    observed = set()
    for table in series.values():
        observed |= set(table["values"])
    first = args.first if args.first is not None else min(observed)
    last = args.last if args.last is not None else max(observed)
    span = [e for e in range(first, last + 1) if e in observed]
    if not span:
        raise SystemExit(f"exchange {first}..{last} 范围内没有记录")

    deployment = report.get("deployment", {}).get("id", "?")
    print(f"报告 {args.report}｜部署 {deployment}｜轴 {args.axis}｜{args.phase}"
          f"｜exchange {span[0]}..{span[-1]}")
    print("发=主站写进帧 (RxPDO)，报=驱动器回报 (TxPDO)；同一行就是同一拍。")

    if args.step:
        render(series, span[::args.step], "—— 逐拍抽样 ——")
        return 0

    # 变化点只看离散字段（状态字/控制字/模式/目标）。模拟量 6064/606C/6077 逐拍都在动，
    # 一起进来就把"变化点"变成"每一拍"，反而看不见那几处真正的翻转。要看模拟量用 --step。
    changes = []
    previous = {}
    for exchange in span:
        current = {key: table["values"].get(exchange) for key, table in series.items()}
        if not changes or any(current.get(key) != previous.get(key) for key in DISCRETE_OBJECTS
                              if key in series):
            changes.append(exchange)
        previous = current
    print(f"—— 变化点 {len(changes)} 处（离散字段相对上一拍变了）——")
    render(series, changes, "跟踪字段：")

    # 补一句因果摘要：目标第一次非零的那一拍，和位置开始移动的那一拍，谁在前。
    target = series.get("0x60FF:00")
    position = series.get("0x6064:00")
    if target and position:
        nonzero = [e for e, v in sorted(target["values"].items()) if v != 0]
        ordered = sorted(position["values"])
        moves = [ordered[0]]
        for exchange in ordered[1:]:
            if position["values"][exchange] != position["values"][moves[-1]]:
                moves.append(exchange)
        print("\n—— 先后 ——")
        print(f"  60FF 首次非零：{nonzero[0] if nonzero else '（全程为 0）'}")
        print(f"  6064 首次变化：{moves[1] if len(moves) > 1 else '（全程不变）'}"
              f"（{position['values'][moves[0]]} -> "
              f"{position['values'][moves[1]] if len(moves) > 1 else '-'}）")
        if len(moves) > 1:
            print(f"  6064 末值：{position['values'][moves[-1]]}"
                  f"（净位移 {position['values'][moves[-1]] - position['values'][moves[0]]} counts）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
