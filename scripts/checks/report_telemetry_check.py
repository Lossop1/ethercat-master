#!/usr/bin/env python3
"""报告里慢速遥测那一节怎么印，离线验。

    python3 scripts/checks/report_telemetry_check.py
    python3 scripts/checks/report_telemetry_check.py --mutate   # 证明这检查抓得住"照旧印 0"

为什么值得单独验：这几个量（温度/母线电压/电流）是 50 ms 一轮的 SDO 轮询，停机信号会
截断最后一轮，于是报告里那几个 0 有两种来历——真读到 0，或者压根没读到。数字上两者
分不开（P11.8 就是这件事：桥臂里轴 2~5 全 0，而温度其实在 180 ms 前读到过）。分得开
的只有 slow_telemetry_read 这个标志，而**看这个标志的那段代码在脚本里**：它写歪了不会
崩、不会报错，只会继续安静地印出 0.0°C——正是"5 号电机有点烫"最需要看对的那个数。

检查喂的是一份合成报告：一根轴读到 42.0°C、一根轴真读到 0.0°C、一根轴从没读到。
后两根必须印得**不一样**，这才是判据。
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTRACTOR = os.path.join(ROOT, "scripts", "analysis", "extract_report.py")

# 变异：把"认标志"那句拿掉，退回到"照旧印数字"的老写法。
MUTATE_FROM = 'if not flag(b"slow_telemetry_read"):'
MUTATE_TO = 'if False:  # 变异：不认标志'

PASSED = 0
FAILED = 0


def ok(what):
    global PASSED
    PASSED += 1
    print(f"ok    {what}")


def bad(what, expected, actual):
    global FAILED
    FAILED += 1
    print(f"FAIL  {what}\n      期望：{expected}\n      实际：{actual}")


def make_report(path):
    """合成一份单行报告：三根轴，两种"0"各一根。"""

    def axis(mosfet, motor, voltage, current, read, age_ns):
        return (f'"runtime":{{"status_word":"0x1237","control_word":"0x000F",'
                f'"actual_current":{current},"dc_link_voltage":{voltage},'
                f'"mosfet_temperature":{mosfet},"motor_temperature":{motor},'
                f'"slow_telemetry_read":{"true" if read else "false"},'
                f'"slow_telemetry_age_ns":{age_ns}}}')

    body = (b'"cycle_count":60000,"op_reached":true,'
            + axis(420, 350, 48100, -30, True, 182000000).encode()   # 读到 42.0°C
            + b","
            + axis(0, 0, 0, 0, True, 5000000).encode()               # 真读到 0
            + b","
            + axis(0, 0, 0, 0, False, 0).encode())                   # 从没读到
    with open(path, "wb") as handle:
        handle.write(body)


def run(extractor):
    with tempfile.TemporaryDirectory() as tmp:
        report = os.path.join(tmp, "report.json")
        make_report(report)
        # 显式 utf-8：子进程的输出是中文，而 Windows 上默认按 GBK 解，会在这里炸掉
        # ——检查的失败长相变成"检查自己崩了"，比判错还难查。
        proc = subprocess.run([sys.executable, extractor, report],
                              capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
    return proc.returncode, proc.stdout


def telemetry_lines(out):
    """抓出那一节里的轴行。"""
    block = out.split("慢速遥测", 1)[-1]
    return [line for line in block.splitlines() if line.strip().startswith("轴")]


def check(extractor):
    code, out = run(extractor)
    if code != 0:
        bad("抽取器退出码为 0", "0", f"{code}；输出尾部：{out[-400:]}")
        return
    ok("抽取器退出码为 0（合成报告没让它崩）")

    lines = telemetry_lines(out)
    if len(lines) == 3:
        ok("三根轴各印一行")
    else:
        bad("三根轴各印一行", "3 行", f"{len(lines)} 行：{lines}")
        return

    first = lines[0]
    for want, label in (("42.0°C", "轴1 mosfet=42.0°C"),
                        ("35.0°C", "轴1 motor=35.0°C"),
                        ("48.1V", "轴1 母线=48.1V"),
                        ("-0.030A", "轴1 电流=-0.030A"),
                        ("182.0 ms", "轴1 给出读数年龄 182.0 ms")):
        if want in first:
            ok(label)
        else:
            bad(label, want, first)

    second = lines[1]
    if "0.0°C" in second:
        ok("轴2 真读到 0 就印 0.0°C（与没读到的印法不同）")
    else:
        bad("轴2 真读到 0 就印 0.0°C", "0.0°C", second)

    third = lines[2]
    if "—" in third and "°C" not in third:
        ok("轴3 从没读到就印「—」，一个温度数字都不印")
    else:
        bad("轴3 从没读到就印「—」", "含「—」且不含 °C", third)
    if re.search(r"\d\.\d", third):
        bad("轴3 不得出现任何小数读数", "没有 d.d 形状的数字", third)
    else:
        ok("轴3 没有把 0 印成读数")


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        if not os.path.exists(EXTRACTOR):
            print(f"找不到 {EXTRACTOR}", file=sys.stderr)
            return 2
        with open(EXTRACTOR, encoding="utf-8") as handle:
            source = handle.read()
        if MUTATE_FROM not in source:
            print("变异点找不到——抽取器里那句认标志的写法变了，这个检查得跟着改。",
                  file=sys.stderr)
            return 2
        with tempfile.TemporaryDirectory() as tmp:
            mutated = os.path.join(tmp, "extract_report_mutated.py")
            with open(mutated, "w", encoding="utf-8") as handle:
                handle.write(source.replace(MUTATE_FROM, MUTATE_TO, 1))
            check(mutated)
        if FAILED:
            print(f"\n变异被抓住：{FAILED} 项不符（这正是要证明的）")
            return 0
        print("\n变异没被抓住——这个检查抓不住“照旧印 0”那种写法，等于摆设")
        return 1

    extractor = os.path.abspath(argv[0]) if argv else EXTRACTOR
    if not os.path.exists(extractor):
        print(f"找不到 {extractor}", file=sys.stderr)
        return 2
    check(extractor)
    print(f"\n报告慢速遥测渲染：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
