#!/usr/bin/env python3
"""A/B 对比：从若干份报告里抽同一组关键指标，横着排开方便比对。

报告很大（每份约 265 MB），逐个加载、抽完即弃，避免同时驻留多份。
accesses 与各路 samples 不参与输出——它们占了报告 99% 的体积。
"""
import json
import re
import sys

# 关心的字段名（正则，匹配 key 本身，不匹配路径）
WANTED = re.compile(
    r"^(status_code|fault_latched|safe_state_reached|result_status"
    r"|wkc_error_count|wkc_no_frame_count|wkc_max_consecutive_errors"
    r"|wkc_no_frame_total|wkc_mismatch_total"
    r"|first_mismatch_exchange|first_mismatch_wkc"
    r"|wkc_no_frame_first_exchange|wkc_no_frame_first_wkc"
    r"|first_deadline_missed_exchange|deadline_missed_count"
    r"|error_counter_read_attempt_count|error_counter_skip_count"
    r"|error_counter_read_count|tail_max_error_counter_ns"
    r"|tail_max_receive_ns|tail_max_mailbox_ns"
    r"|frame_interval_max_ns|frame_interval_max_exchange"
    r"|frame_interval_gap_count|first_frame_interval_gap_exchange"
    r"|first_mismatch_frame_interval_ns|deadline_miss_frame_interval_ns"
    r"|tail_uncovered_max_ns|tail_uncovered_max_exchange"
    r"|deadline_miss_tail_uncovered_ns"
    r"|shutdown_prologue_gap_ns|safe_output_sent)$"
)
SKIP = {"accesses", "samples", "records", "entries"}


def walk(node, path, out):
    if isinstance(node, dict):
        for key, value in node.items():
            if key in SKIP:
                continue
            if isinstance(value, (dict, list)):
                walk(value, f"{path}.{key}" if path else key, out)
            elif WANTED.match(key):
                out[f"{path}.{key}" if path else key] = value
    elif isinstance(node, list):
        # 列表只取首元素做样本，避免把逐周期数组整个展开
        if node:
            walk(node[0], f"{path}[0]", out)


def main():
    merged = {}
    for path in sys.argv[1:]:
        with open(path) as handle:
            data = json.load(handle)
        data.pop("accesses", None)
        out = {}
        walk(data, "", out)
        tag = path.rsplit("/", 1)[-1]
        merged[tag] = out
        del data

    keys = sorted({k for out in merged.values() for k in out})
    tags = list(merged)
    width = max(len(k) for k in keys) if keys else 10
    print(" " * width + " | " + " | ".join(t[:34].rjust(34) for t in tags))
    for key in keys:
        cells = []
        for tag in tags:
            value = merged[tag].get(key, "-")
            cells.append(str(value)[:34].rjust(34))
        print(key.ljust(width) + " | " + " | ".join(cells))


if __name__ == "__main__":
    main()
