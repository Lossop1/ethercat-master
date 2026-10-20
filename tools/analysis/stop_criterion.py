#!/usr/bin/env python3
"""从一轮实跑报告推出停机判据掩码（报告离线判读工具）。

用法：python3 tools/analysis/stop_criterion.py <report.json> [report.json ...]

停机判据是设备侧的事实：主站发完安全停机帧后，驱动器把 0x6041 停在哪个值，
只有这一台设备知道。策略档里的 safe_stop_status_mask/value 就是把这个值写成
判据；不填，主站只按标准 CiA 402 判据（未使能/未就绪）判，而不少驱动器的 PDO
状态字在停机循环里到不了那两个状态，于是第一轮总是"停机未确认"。

这个脚本只做一件事：把同一轴"使能时"和"停住时"的两个 0x6041 摆出来，
给出掩码候选。判据仍然由人确认后写进策略档——掩码填错不会让轴更危险，
但会让主站把"没停"当成"停了"，所以这里不自动写文件。

数据来源：
  - schema 24 起，axes[].runtime.operation_enabled_status_word 是首次使能时读到的值。
  - 更早的报告没有这一项，退到 shutdown_cycles 的逐拍采样里最后一个 bit2=1 的状态字。
  - 停住时的值取 axes[].runtime.status_word（PDO 最后一拍），**不取**
    shutdown_cia402.statusword：后者是停机循环之后用邮箱写 0x6040=0 再读的值，
    往往已经到 Switch-on-disabled，而停机循环按 PDO 判定时根本看不到它。

退出码：0 至少有一轴的候选可给出；1 用法错误；2 报告里没有可用证据。
"""

import argparse
import json
import re
import sys

OP_ENABLED_BIT = 0x0004


def _file_stem(profile_id):
    """与 tools/gen/emaster_config.py 的 _file_stem 同一规则，策略档文件名由它决定。"""
    return re.sub(r"[^a-z0-9]+", "_", profile_id.lower()).strip("_")


def _hex(value):
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text, 16) if text.lower().startswith("0x") else int(text, 10)
        except ValueError:
            return None
    return None


def _fmt(value):
    return "0x%04X" % value if isinstance(value, int) else "?"


def enabled_word_from_samples(report, axis_index):
    """旧报告的回退：停机循环里最后一个 bit2=1 的状态字就是使能态。"""
    trace = report.get("shutdown_cycles") or {}
    rows = list(trace.get("samples") or [])
    last = trace.get("last")
    if isinstance(last, dict):
        rows.append(last)
    found = None
    for sample in rows:
        axes = sample.get("axes") or []
        if axis_index >= len(axes):
            continue
        word = _hex(axes[axis_index].get("status_word"))
        if word is not None and word & OP_ENABLED_BIT:
            found = word
    return found


def derive(report):
    """逐轴返回 (axis 序号, profile_id, 使能字, 停住字, mask, value, 来源说明)。"""
    axes = report.get("axes") or []
    rows = []
    for index, axis in enumerate(axes):
        runtime = axis.get("runtime") or {}
        profile_id = axis.get("device_profile_id") or "?"
        stopped = _hex(runtime.get("status_word"))
        enabled = None
        source = ""
        if runtime.get("operation_enabled_seen"):
            enabled = _hex(runtime.get("operation_enabled_status_word"))
            if enabled is not None:
                source = "报告字段"
        if enabled is None:
            enabled = enabled_word_from_samples(report, index)
            if enabled is not None:
                source = "停机循环采样（旧 schema）"
        if enabled is None or stopped is None:
            rows.append((index, profile_id, enabled, stopped, None, None, "缺少使能或停住读数"))
            continue
        if not enabled & OP_ENABLED_BIT:
            rows.append((index, profile_id, enabled, stopped, None, None,
                         "使能读数 bit2=0，不是操作使能态"))
            continue
        if stopped & OP_ENABLED_BIT:
            rows.append((index, profile_id, enabled, stopped, None, None,
                         "停住读数 bit2=1，停机循环里轴仍在操作使能态"))
            continue
        mask = enabled ^ stopped
        value = stopped & mask
        if mask == 0:
            rows.append((index, profile_id, enabled, stopped, None, None,
                         "使能字与停住字相同，没有可用的差异"))
            continue
        rows.append((index, profile_id, enabled, stopped, mask, value, source))
    return rows


def main():
    parser = argparse.ArgumentParser(
        description="从实跑报告推出 safe_stop_status_mask/value 的候选值")
    parser.add_argument("reports", nargs="+", help="运行报告 JSON")
    args = parser.parse_args()

    any_derived = False
    for path in args.reports:
        with open(path, encoding="utf-8") as handle:
            report = json.load(handle)
        result = report.get("result") or {}
        print("=" * 78)
        print("%s  schema=%s" % (path, report.get("schema_version")))
        print("  本轮：判据=%s 安全状态到达=%s 停机故障码=%s"
              % (report.get("shutdown_stop_criterion", "?"),
                 result.get("safe_state_reached"),
                 result.get("shutdown_stop_fault")))

        rows = derive(report)
        groups = {}
        for index, profile_id, enabled, stopped, mask, value, note in rows:
            if mask is None:
                print("  轴%d [%s]：无法推导——%s（使能 %s / 停住 %s）"
                      % (index + 1, profile_id, note, _fmt(enabled), _fmt(stopped)))
                continue
            print("  轴%d [%s]：使能 %s → 停住 %s；候选 mask=%s value=%s（来源：%s）"
                  % (index + 1, profile_id, _fmt(enabled), _fmt(stopped),
                     _fmt(mask), _fmt(value), note))
            groups.setdefault(profile_id, set()).add((mask, value))
            any_derived = True

        for profile_id, candidates in groups.items():
            if len(candidates) != 1:
                print("  [%s] 各轴候选不一致，先查为什么同型号停机状态不同，不要选一个填进去"
                      % profile_id)
                continue
            mask, value = next(iter(candidates))
            policy = _file_stem(profile_id)
            print("  [%s] 策略档 config/device_policies/%s.json：" % (profile_id, policy))
            print('    "safe_stop_status_mask": "%s",' % _fmt(mask))
            print('    "safe_stop_status_value": "%s",' % _fmt(value))

        if not any_derived:
            print("  本报告没有给出候选。至少要有一轮：轴真正使能过、停机循环解出了停住状态字。")
        else:
            print("  写进策略档后重跑 add-device 与 build（只跑 build 不会把掩码带进设备档），"
                  "再跑一轮确认停机确认通过。")

    return 0 if any_derived else 2


if __name__ == "__main__":
    sys.exit(main())
