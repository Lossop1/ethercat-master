#!/usr/bin/env python3
"""报告里慢速遥测那一节怎么印，离线验。

    python3 scripts/checks/report_telemetry_check.py
    python3 scripts/checks/report_telemetry_check.py --mutate   # 两处对照，证明抓得住"照旧印 0"

为什么值得单独验：这几个量（温度/母线电压/电流）是 50 ms 一轮的 SDO 轮询，停机信号会
截断最后一轮，于是报告里那几个 0 有两种来历——真读到 0，或者压根没读到。数字上两者
分不开（P11.8 就是这件事：桥臂里轴 2~5 全 0，而温度其实在 180 ms 前读到过）。分得开
的只有 slow_telemetry_read 这个标志，而**看这个标志的那段代码在脚本里**：它写歪了不会
崩、不会报错，只会继续安静地印出 0.0°C——正是"5 号电机有点烫"最需要看对的那个数。

检查喂的是一份合成报告：一根轴读到 42.0°C、一根轴真读到 0.0°C、一根轴从没读到。
后两根必须印得**不一样**，这才是判据。

运行期 603F（驱动器错误码）走同一条判据、同一份合成报告，但**标志是字段级的**：
温度读到而错误码那一轮 SDO 超时是常事，共用一个轴级标志会把"从没读到"印成
`0x0000`，看着像"驱动器说没事"——而 P12.17 里五轴 AL 侧一律 state=8/code=0、
总线全干净，603F 里有没有东西正是关键一问，印错就把线索改没了。
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTRACTOR = os.path.join(ROOT, "scripts", "analysis", "extract_report.py")

# 变异：把"认标志"那句拿掉，退回到"照旧印数字"的老写法。两处守卫是同一类错法，但
# 由两组断言分别看着（慢速遥测 / 603F），所以要各证一次：只拆一处的变异若把两半都
# 判红，那说明这两组断言里有一组其实没在盯自己的那一列。
MUTATE_TELEMETRY_FROM = 'if not field_flag(block, b"slow_telemetry_read"):'
MUTATE_CODE_FROM = 'if not field_flag(block, b"runtime_error_code_read"):'
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
    """合成一份单行报告：三根轴，两种"0"各一根。

    同时喂上运行期 603F 那一列（P12.17）：轴 1 读到 0x2310、轴 2 真读到 0x0000、
    轴 3 从没读到。后两根必须印得**不一样**——这与慢速遥测是同一条判据，但标志是
    字段级的，不能借用温度那个轴级标志。
    """

    def axis(mosfet, motor, voltage, current, read, age_ns, code, code_read,
             code_age_ns):
        return (f'"runtime":{{"status_word":"0x1237","control_word":"0x000F",'
                f'"actual_current":{current},"dc_link_voltage":{voltage},'
                f'"mosfet_temperature":{mosfet},"motor_temperature":{motor},'
                f'"slow_telemetry_read":{"true" if read else "false"},'
                f'"slow_telemetry_age_ns":{age_ns},'
                f'"runtime_error_code_read":{"true" if code_read else "false"},'
                f'"runtime_error_code":"0x{code:04X}",'
                f'"runtime_error_code_age_ns":{code_age_ns}}}')

    body = (b'"cycle_count":60000,"op_reached":true,'
            # 轴 1：读到 42.0°C，603F 读到 0x2310
            + axis(420, 350, 48100, -30, True, 182000000, 0x2310, True, 182000000).encode()
            + b","
            # 轴 2：温度真读到 0，603F 真读到 0x0000
            + axis(0, 0, 0, 0, True, 5000000, 0x0000, True, 5000000).encode()
            + b","
            # 轴 3：两样都没读到
            + axis(0, 0, 0, 0, False, 0, 0x0000, False, 0).encode())
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


def section(out, title):
    """取一个 `== 标题 ==` 小节。

    必须**在下一节处截断**：抽取器的输出是一节接一节，而每一节的轴行都以「轴」开头。
    只按标题往后切的话，后面几节的轴行会混进来——本文件加了 603F 那一节之后，
    慢速遥测的"三根轴各印一行"会变成"六根轴"，检查自己先红了。
    """
    tail = out.split(title, 1)[-1]
    return tail.split("\n== ", 1)[0]


def telemetry_lines(out):
    """抓出慢速遥测那一节里的轴行。"""
    return [line for line in section(out, "慢速遥测").splitlines()
            if line.strip().startswith("轴")]


def error_code_lines(out):
    """抓出运行期 603F 那一节里的轴行。"""
    return [line for line in section(out, "运行期 603F").splitlines()
            if line.strip().startswith("轴")]


def check(extractor):
    code, out = run(extractor)
    if code != 0:
        bad("抽取器退出码为 0", "0", f"{code}；输出尾部：{out[-400:]}")
        return
    ok("抽取器退出码为 0（合成报告没让它崩）")

    # 603F 那一列先判：慢速遥测那半在行数不对时会提前返回，放在后面会被它连累着跳过。
    check_error_codes(out)

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


def check_error_codes(out):
    """运行期 603F 那一列（P12.17）：与慢速遥测同一条判据，但标志是字段级的。

    现场最要紧的一句：驱动器 AL 侧干净（state=8/code=0）、总线也干净时，603F 里
    有没有东西是唯一能指的线索。把"没读到"印成 0x0000 就等于说"驱动器说没事"。
    """
    lines = error_code_lines(out)
    if len(lines) == 3:
        ok("603F：三根轴各印一行")
    else:
        bad("603F：三根轴各印一行", "3 行", f"{len(lines)} 行：{lines}")
        return

    if "0x2310" in lines[0]:
        ok("603F：轴1 读到 0x2310 就印 0x2310（印成 0x0000 说明取值那步取错了形态）")
    else:
        bad("603F：轴1 印出读到的 0x2310", "0x2310", lines[0])
    if "182.0 ms" in lines[0]:
        ok("603F：轴1 给出读数年龄")
    else:
        bad("603F：轴1 给出读数年龄", "182.0 ms", lines[0])

    if "0x0000" in lines[1]:
        ok("603F：轴2 真读到 0x0000 就印 0x0000（与没读到的印法不同）")
    else:
        bad("603F：轴2 真读到 0x0000", "0x0000", lines[1])

    if "—" in lines[2] and "0x" not in lines[2]:
        ok("603F：轴3 从没读到就印「—」，一个十六进制数都不印")
    else:
        bad("603F：轴3 从没读到就印「—」", "含「—」且不含 0x", lines[2])


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--mutate":
        if not os.path.exists(EXTRACTOR):
            print(f"找不到 {EXTRACTOR}", file=sys.stderr)
            return 2
        with open(EXTRACTOR, encoding="utf-8") as handle:
            source = handle.read()
        for anchor in (MUTATE_TELEMETRY_FROM, MUTATE_CODE_FROM):
            if anchor not in source:
                print("变异点找不到——抽取器里那句认标志的写法变了，这个检查得跟着改。",
                      file=sys.stderr)
                return 2
        global PASSED, FAILED
        mutants = {
            "慢速遥测不认标志": source.replace(MUTATE_TELEMETRY_FROM, MUTATE_TO, 1),
            "603F 不认标志": source.replace(MUTATE_CODE_FROM, MUTATE_TO, 1),
        }
        for name, text in mutants.items():
            PASSED = 0
            FAILED = 0
            print(f"--- 变异：{name} ---")
            with tempfile.TemporaryDirectory() as tmp:
                mutated = os.path.join(tmp, "extract_report_mutated.py")
                with open(mutated, "w", encoding="utf-8") as handle:
                    handle.write(text)
                check(mutated)
            if FAILED:
                print(f"      {FAILED} 项不符（抓住了）")
            else:
                print("      一项都没判红——变异混过去了")
                return 1
        print("\n两个变异都被抓住（这正是要证明的）")
        return 0

    extractor = os.path.abspath(argv[0]) if argv else EXTRACTOR
    if not os.path.exists(extractor):
        print(f"找不到 {extractor}", file=sys.stderr)
        return 2
    check(extractor)
    print(f"\n报告慢速遥测渲染：通过 {PASSED} 项，失败 {FAILED} 项")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
