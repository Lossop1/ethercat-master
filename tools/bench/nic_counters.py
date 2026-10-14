#!/usr/bin/env python3
"""网卡收发计数的快照与差分。

用法:
    python3 tools/bench/nic_counters.py <输出文件> [--interface NAME]
    python3 tools/bench/nic_counters.py --diff <前一份> <后一份>

为什么要有它。**这是目前唯一一个在主站软件之外的观测点**，用来分辨"帧没回来"
（链路/从站侧）与"帧回来了但主站没读"（主机侧）：

    发出去的帧数 = tx_packets，回到网卡的帧数 = rx_packets。
    帧只要到达网卡就算 rx，**哪怕它到晚了**——比如 300us 才到而 SOEM 在 250us 就超时了。
    所以 tx - rx 是"真的没回来的帧"，"回来晚了"不会计入这个差。

    tx 正常、rx == tx  ->  帧都回来了，是主站没及时读  ->  主机侧
    tx 正常、rx <  tx  ->  有帧没回来                ->  链路/从站侧
    tx 本身少了        ->  主站根本没发              ->  主站被调度走了

前置条件（2026-10-08 实测确认）：enp49s0 无 IP、无路由、NetworkManager 标它
unavailable，空闲 10 秒 tx/rx 增量为 0 —— 接口上只有 EtherCAT 帧，差分才干净。
**换了接口或改了网络配置之后必须重测这一条**，否则计数会混进别的流量，差分给出的
结论就是错的，而且错得看不出来。

用 sysfs 而不是 ethtool：一次读文件，不起子进程。ethtool 独有而 sysfs 没有的那几个
（rx_missed、align_errors）判别用不上——要区分的只是 tx 与 rx。

退出码：0 = 正常；1 = 接口不存在或读不了；2 = 用法错。
"""

import argparse
import os
import sys

# 要记的计数。tx/rx 是判别量，其余是"顺带看一眼有没有物理层痕迹"。
COUNTERS = (
    "tx_packets", "rx_packets", "tx_errors", "rx_errors",
    "rx_crc_errors", "rx_frame_errors", "rx_length_errors", "rx_missed_errors",
    "rx_fifo_errors", "rx_dropped", "rx_nohandler",
    "tx_carrier_errors", "tx_aborted_errors", "tx_fifo_errors",
)
STATS = "/sys/class/net/%s/statistics/%s"


def read_counters(interface):
    """读一次快照，返回 (dict, 错误说明)。读不到的项写 na，不写 0 ——
    0 在这些计数上是合法值（"没发生过"），与"读不到"必须分得开。"""
    values = {}
    if not os.path.isdir("/sys/class/net/%s" % interface):
        return values, "接口不存在：%s" % interface
    for name in COUNTERS:
        try:
            with open(STATS % (interface, name), encoding="utf-8") as handle:
                values[name] = int(handle.read().strip())
        except (OSError, ValueError):
            values[name] = None
    return values, None


def format_snapshot(values, interface):
    lines = ["interface=%s" % interface]
    for name in COUNTERS:
        value = values.get(name)
        lines.append("nic_%s=%s" % (name, "na" if value is None else value))
    return lines


def load_snapshot(path):
    values = {}
    interface = "?"
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if not sep:
                    continue
                if key == "interface":
                    interface = value
                elif key.startswith("nic_"):
                    values[key[4:]] = int(value) if value.isdigit() else None
    except OSError as error:
        return None, None, "读不了 %s（%s）" % (path, error)
    return values, interface, None


def diff(before_path, after_path):
    before, interface, error = load_snapshot(before_path)
    if error:
        print("错误：%s" % error, file=sys.stderr)
        return 2
    after, _, error = load_snapshot(after_path)
    if error:
        print("错误：%s" % error, file=sys.stderr)
        return 2

    delta = {}
    for name in COUNTERS:
        if before.get(name) is None or after.get(name) is None:
            delta[name] = None
        else:
            delta[name] = after[name] - before[name]

    print("接口=%s" % interface)
    for name in COUNTERS:
        value = delta[name]
        if value:
            print("  %-22s %d" % (name, value))

    tx = delta.get("tx_packets")
    rx = delta.get("rx_packets")
    if tx is None or rx is None:
        print("判定：计数读不到，无法判定")
        return 0
    lost = tx - rx
    print("  发=%d 回=%d 差=%d" % (tx, rx, lost))
    if lost > 0:
        print("判定：**有 %d 帧没回到网卡** -> 链路/从站侧" % lost)
    elif tx == 0:
        print("判定：本轮一帧都没发（主站没跑起来？）")
    else:
        # 这一条是全部意义所在：帧全回来了而仍有收包超时，说明问题在主机侧调度，
        # 不在线上。它把"链路/从站"与"主机"这两个修法相反的候选一次劈开。
        print("判定：发出的帧全部回到了网卡 -> 帧没丢；若仍有收包超时，那是主机侧")
    return 0


def main():
    parser = argparse.ArgumentParser(description="网卡收发计数快照 / 差分")
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--interface", default="enp49s0")
    parser.add_argument("--diff", action="store_true", help="把两个快照相减")
    args = parser.parse_args()

    if args.diff:
        if len(args.paths) != 2:
            print(__doc__)
            return 2
        return diff(args.paths[0], args.paths[1])

    values, error = read_counters(args.interface)
    if error:
        print("错误：%s" % error, file=sys.stderr)
        return 1
    output = args.paths[0]
    directory = os.path.dirname(os.path.abspath(output))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(output, "w", encoding="utf-8") as handle:
        handle.write("\n".join(format_snapshot(values, args.interface)) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
