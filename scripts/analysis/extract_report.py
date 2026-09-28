#!/usr/bin/env python3
"""在 Pi 上就地抽取大报告里的关键字段（不下载整个 JSON）。

报告是单行巨型 JSON（100MB 量级），本地 grep 会超时；这里按定界模式抽取。
用法: python3 scripts/analysis/extract_report.py <report.json>
"""

import re
import sys


def field_int(block, key, default=None):
    """从一轴的 runtime 块里取一个整数。取不到返回 default——调用方要能区分
    "值是 0"与"没有这个字段"，所以默认值由调用方给，不在这里替它编一个 0。"""
    hit = re.search(rb'"%s":(-?\d+)' % key, block)
    return int(hit.group(1)) if hit else default


def field_hex(block, key, default=None):
    """报告里的错误码/状态字印成**带引号的十六进制串**（`"0x2310"`），不是数字。

    用 field_int 去取它会一无所获、然后安静地拿到调用方的默认值——603F 于是永远印成
    0x0000，看着像"驱动器说没事"。这一列恰恰是 P12.17 要问的东西。
    """
    hit = re.search(rb'"%s":"0[xX]([0-9A-Fa-f]+)"' % key, block)
    return int(hit.group(1), 16) if hit else default


def field_flag(block, key):
    hit = re.search(rb'"%s":(true|false)' % key, block)
    return hit is not None and hit.group(1) == b"true"


def main():
    path = sys.argv[1]
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"报告 {path} 大小 {len(data)} 字节")

    def shown(pattern, label, limit=8):
        found = re.findall(pattern, data)
        print(f"\n== {label} ({len(found)}) ==")
        for item in found[:limit]:
            text = item if isinstance(item, bytes) else item[0]
            print("  " + text.decode("utf-8", "replace"))

    shown(rb'"shutdown_al":\{[^}]*\}', "停机后 AL 快照")
    shown(rb'"shutdown_al_entry":\{[^}]*\}', "停机入口 AL 快照")
    shown(rb'"shutdown_al_pre_stop":\{[^}]*\}', "安全停机前 AL 快照")
    shown(rb'"shutdown_observer_join_ns":\d+', "停止观测线程耗时 ns")
    shown(rb'"sm2_diagnostic":\{[^}]*\}', "SM2 同步诊断（驱动器 1C32）", 4)
    shown(rb'"sm3_diagnostic":\{[^}]*\}', "SM3 同步诊断（驱动器 1C33）", 4)

    for key in (rb'"status_code":\d+', rb'"control_state":\d+', rb'"safe_output_sent":\w+',
                rb'"safe_state_reached":\w+', rb'"diagnostic_preop_reached":\w+',
                rb'"restore_init_succeeded":\w+', rb'"sync0_disabled":\w+',
                rb'"wkc_error_count":\d+', rb'"wkc_max_consecutive_errors":\d+',
                rb'"cycle_count":\d+', rb'"expected_wkc":\d+', rb'"actual_wkc":-?\d+',
                rb'"omitted_pdo_samples":\d+', rb'"all_axes_enabled_reached":\w+',
                rb'"op_reached":\w+', rb'"safe_op_reached":\w+',
                rb'"motion_completed":\w+', rb'"cycle_deadline_missed":\w+'):
        found = re.findall(key, data)
        if found:
            print(f"{key.decode():40s} {[v.decode() for v in found[:6]]}")

    print("\n== first_cycle_failure ==")
    hit = re.search(rb'"first_cycle_failure":\{.*?\}\}', data, re.S)
    print("  " + (hit.group(0).decode("utf-8", "replace") if hit else "null"))

    print("\n== 审计阶段计数 ==")
    for phase in (b"preop_configuration", b"safeop_initialization", b"operation_request",
                  b"cyclic_operation", b"safe_stop", b"final_diagnostic"):
        print(f"  {phase.decode():24s} {len(re.findall(phase, data))}")

    print("\n== 观测线程样本 ==")
    for key in (rb'"sdo_current_read":\w+', rb'"sdo_voltage_read":\w+',
                rb'"sdo_mosfet_temp_read":\w+', rb'"sdo_motor_temp_read":\w+',
                rb'"sdo_motor_speed_read":\w+', rb'"sdo_speed_command_read":\w+',
                rb'"sdo_read_count":\d+', rb'"final_diagnostic_success_count":\d+',
                rb'"mode_command_sdo_read":\w+', rb'"following_error_read":\w+',
                rb'"sync0_late_count":\d+',
                rb'"first_sync0_late_exchange":\d+',
                rb'"last_sync0_late_exchange":\d+'):
        found = re.findall(key, data)
        if found:
            print(f"  {key.decode():38s} {[v.decode() for v in found[:6]]}")

    print("\n== 每轴时序统计 ==")
    for item in re.findall(rb'"timing":\{[^}]*\}', data):
        print("  " + item.decode("utf-8", "replace"))

    # 慢速遥测（温度/母线电压/电流）是 50 ms 一轮的 SDO 轮询，停机信号会截断最后一轮。
    # 报告里那几个 0 因此有两种来历：真读到 0，或者压根没读到（P11.8）。这两者在数字上
    # 分不开，只有 slow_telemetry_read 分得开——所以这一节只认那个标志，认不到就印「—」。
    # 读到过的还要给出读数年龄：温度是慢变量，180 ms 前的读数与 3 s 前的读数可信度不同。
    print("\n== 慢速遥测（「—」= 这一轴整场都没读到过，不是 0）==")
    for index, block in enumerate(re.findall(rb'"runtime":\{[^}]*\}', data)):

        def scaled(key, divisor, unit, digits):
            value = field_int(block, key)
            return "—" if value is None else f"{value / divisor:.{digits}f}{unit}"

        if not field_flag(block, b"slow_telemetry_read"):
            print(f"  轴{index + 1} mosfet=— motor=— 母线=— 电流=—"
                  f"   （没读到）")
            continue
        print(f"  轴{index + 1} mosfet={scaled(b'mosfet_temperature', 10.0, '°C', 1)}"
              f" motor={scaled(b'motor_temperature', 10.0, '°C', 1)}"
              f" 母线={scaled(b'dc_link_voltage', 1000.0, 'V', 1)}"
              f" 电流={scaled(b'actual_current', 1000.0, 'A', 3)}"
              f"   读到，读数年龄 {field_int(block, b'slow_telemetry_age_ns', 0) / 1e6:.1f} ms"
              f"（0 点 = 报告回填）")

    # 运行期 603F（驱动器错误码）。与上面同一条约定（P11.8），但**标志是字段级的**：
    # 温度读到、而错误码那一轮 SDO 超时是常事，共用一个轴级标志会把"从没读到"印成
    # 0x0000。这一列对 P12.17 直接有用——那次五轴 AL 侧一律 state=8/code=0、总线全干净，
    # 603F 里有没有东西就是关键一问；把它印成 0x0000 等于把线索改没。
    print("\n== 运行期 603F（「—」= 从没读到过，不是 0x0000）==")
    for index, block in enumerate(re.findall(rb'"runtime":\{[^}]*\}', data)):
        if not field_flag(block, b"runtime_error_code_read"):
            print(f"  轴{index + 1} 603F=—    （没读到）")
            continue
        print(f"  轴{index + 1} 603F=0x{field_hex(block, b'runtime_error_code', 0):04X}"
              f"   读到，读数年龄 {field_int(block, b'runtime_error_code_age_ns', 0) / 1e6:.1f} ms"
              f"（0 点 = 报告回填）")


if __name__ == "__main__":
    main()
