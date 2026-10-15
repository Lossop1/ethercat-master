#!/usr/bin/env python3
"""量一条轴真正走多快：把报告里的过程数据运行段还原，再逐拍差分。

用法：python3 tools/analysis/motion_speed.py <report.json> <从站位置> [counts/度]

为什么单独一件。审计报告把过程数据压成运行段（值不变的连续拍合成一条），
这是它体积可控的原因，也让它能回答一个别处答不了的问题：**主站每拍写的那个数，
与驱动器每拍走的那个数，各是多少**。

**按对象分块打印，不假设模式**。同一条命令口在不同模式下写的对象不是一个：
CSP 写 0x607A、CSV 写 0x60FF、CST 写 0x6071。拿 CSP 的对象去量 CSV 的报告，
得到的是"0 counts，很稳"——因为那个对象在 CSV 下压根没被写。所以这里扫该轴的
**全部**过程数据对象，按出现顺序列出来；只有一段（恒定的那些）就直说恒定。

两级读数缺一不可：

    目标那一列  平台时长就是目标的更新周期。中位 10 拍 = 100 Hz 更新，
                台阶 32 counts / 10 拍 = 3200 counts/s。这是**平均**。
    实际那一列  逐拍增量。中位与最大值的差，就是"平均"与"手感"的差。

实测（2026-10-15，良质 12 轴 CSP，--rate 100，15° / 6 s 单程）：命令平均
2.51 °/s，驱动器实际每拍中位 3.92 °/s、峰值 14.91 °/s —— 差 6 倍。只按参数
算出的那个平均值会低报到让人以为"很慢"。这一件就是用来防止这种低报的。

**流式读**，不 json.load：报告可到 500 MB，整份读进内存会把台架机吃掉。
counts/度 缺省时不印角度、只印 counts/s —— 那个换算来自设备 ESI，不在这里猜。
"""
import re
import sys

# CiA 402 的对象索引 → 人读的名字。与主站侧同一张标准表，不是某个型号的。
NAMES = {
    "0x6040": "控制字",
    "0x6041": "状态字",
    "0x6060": "模式",
    "0x6061": "模式显示",
    "0x6064": "实际位置",
    "0x606c": "实际速度",
    "0x6071": "目标力矩",
    "0x6077": "实际力矩",
    "0x607a": "目标位置",
    "0x60ff": "目标速度",
}

ENTRY = re.compile(
    r'"phase":"cyclic_operation"[^}]*?'
    r'"slave_position":(\d+),"object":"(0x[0-9A-Fa-f]{4}):(\d+)"'
    r'[^}]*?"value_dec":(-?\d+)[^}]*?'
    r'"first_exchange":(\d+),"last_exchange":(\d+)'
)


def scan(path, slave):
    """一遍流式，取出这条轴全部对象的运行段。"""
    found = {}
    carry = ""
    with open(path, "r", encoding="utf-8") as handle:
        while True:
            chunk = handle.read(1 << 22)
            if not chunk:
                break
            buffer = carry + chunk
            for position, index, sub, value, first, last in ENTRY.findall(buffer):
                if int(position) != slave:
                    continue
                found.setdefault((index.lower(), int(sub)), []).append(
                    (int(value), int(first), int(last))
                )
            # 分块边界会把同一条段切开，末尾留一截接下一块
            carry = buffer[-2048:]
    for runs in found.values():
        runs.sort(key=lambda item: item[1])
        # 重叠读出来的同一段会连着出现两次，元组相等即可去重
        dedup = []
        for run in runs:
            if dedup and dedup[-1] == run:
                continue
            dedup.append(run)
        runs[:] = dedup
    return found


def _median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0


def _rate(counts, cpg):
    if cpg:
        return "%.0f counts/s = %.2f °/s" % (counts, counts / cpg)
    return "%.0f counts/s" % counts


def _label(index, sub):
    name = NAMES.get(index, index)
    return "%s（%s）" % (index.upper().replace("0X", "0x") + (":%02d" % sub if sub else ""), name)


def detail(runs, index, sub, slave, cpg):
    values = [value for value, _, _ in runs]
    plateaus = [last - first + 1 for _, first, last in runs]
    changes = []
    for previous, current in zip(runs, runs[1:]):
        gap = current[1] - previous[2]
        if gap <= 0:
            continue
        # 平台本身也算进跨度：台阶是"停 10 拍再跳 1 拍"，只除跳的那 1 拍就成了假峰值。
        # 峰值那一项反过来只除 gap —— 它要的正是"这 1 拍里变了多少"。
        plateau = previous[2] - previous[1] + 1
        changes.append((abs(current[0] - previous[0]), gap, plateau + gap,
                        current[0] - previous[0]))
    head = "从站%d %s" % (slave, _label(index, sub))
    if not changes:
        print("%s：%d 段，无时间跨度" % (head, len(runs)))
        return
    total = sum(item[2] for item in changes)
    mean = sum(item[0] for item in changes) / total * 1000.0
    peak = max(changes, key=lambda item: item[0] / item[1])
    steps = [item[0] for item in changes]
    print("%s：段数 %d，交换号 %d..%d（%d 拍）"
          % (head, len(runs), runs[0][1], runs[-1][2], runs[-1][2] - runs[0][1]))
    print("  取值 %d .. %d counts" % (min(values), max(values)))
    print("  平台时长 中位 %d 拍，最长 %d 拍" % (_median(plateaus), max(plateaus)))
    print("  段间变化 中位 %d counts，最大 %d counts" % (_median(steps), max(steps)))
    print("  平均速率 %s" % _rate(mean, cpg))
    print("  峰值速率 %s（一次变化 %+d counts，跨 %d 拍）"
          % (_rate(peak[0] / peak[1] * 1000.0, cpg), peak[3], peak[1]))


def constant(runs, index, sub, slave):
    value, first, last = runs[0]
    print("从站%d %s：恒定 %d（%d 拍）" % (slave, _label(index, sub), value, last - first + 1))


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    path, slave = argv[0], int(argv[1])
    cpg = float(argv[2]) if len(argv) > 2 else None
    found = scan(path, slave)
    if not found:
        print("从站%d：报告里没有这个过程数据对象" % slave)
        return 1
    for (index, sub), runs in sorted(found.items()):
        if len(runs) < 3:
            constant(runs, index, sub, slave)
        else:
            detail(runs, index, sub, slave, cpg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
