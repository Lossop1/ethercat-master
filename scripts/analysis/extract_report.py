#!/usr/bin/env python3
"""在 Pi 上就地抽取大报告里的关键字段（不下载整个 JSON）。

报告是单行巨型 JSON（100MB 量级），本地 grep 会超时；这里按定界模式抽取。
用法: python3 scripts/analysis/extract_report.py <report.json>
"""

import re
import sys


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


if __name__ == "__main__":
    main()
