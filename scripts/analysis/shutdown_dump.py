#!/usr/bin/env python3
"""停机序言仪表的读出脚本（报告离线判读工具）。

用法：python3 scripts/analysis/shutdown_dump.py <report.json> [report.json ...]

把 schema_version 4 新增的四个块（shutdown_prologue / shutdown_cycles /
observer_stop / thread_schedstat）按判读顺序摊开。这里只做换算和排版，
不下结论：

  序言分段   各标记点相对序言起点的累计位置（ns → ms），以及两次 AL 快照自身耗时
  停机循环   逐拍现场（哪一拍开始 all_axes_safe=false、哪一拍状态字变了）
             以及首次 WKC 不符那一刻的 AL 快照
  观测线程   标志落下 → 被看见 → 循环退出 → 线程返回（把 join 拆成三段）
  线程调度   各线程运行/排队等待的累计值
"""

import json
import sys

PHASE_NAMES = {0: "未知", 1: "邮箱读", 2: "解算写样本", 3: "睡眠片"}
SITE_NAMES = {0: "睡眠片", 1: "邮箱读返回", 2: "读入口/循环顶"}
STATE_NAMES = {
    0: "UNKNOWN", 1: "NOT_READY", 2: "SWITCH_ON_DISABLED", 3: "READY_TO_SWITCH_ON",
    4: "SWITCHED_ON", 5: "OP_ENABLED", 6: "QUICK_STOP", 7: "FAULT_REACTION", 8: "FAULT",
}


def ms(ns):
    return "%.3f" % ((ns or 0) / 1e6)


def coordinate(index, axis, key):
    """每轴 AL 坐标是嵌套对象 {state, status_code}，不是扁平字段。"""
    row = axis.get(key) or {}
    return "轴%d %s/0x%04X" % (index + 1, row.get("state"), row.get("status_code") or 0)


def first_mismatch(index, axis):
    row = axis.get("first_mismatch") or {}
    return "轴%d %s(state=%s/0x%04X)" % (
        index + 1, "已读" if row.get("al_read") else "未读",
        row.get("al_state"), row.get("al_status_code") or 0)


def dump_prologue(report):
    block = report.get("shutdown_prologue") or {}
    if not block:
        print("  序言分段：报告里没有这一段（schema_version 3 的报告？）")
        return
    print("  序言分段（相对起点，start_valid=%s fast_mode=%s）："
          % (block.get("start_valid"), block.get("fast_mode")))
    print("    入口 AL 快照后 %s ms（快照自身 %s ms）"
          % (ms(block.get("mark_after_entry_al_ns")), ms(block.get("entry_al_read_ns"))))
    print("    停观测线程后 %s ms" % ms(block.get("mark_after_join_ns")))
    print("    停机前 AL 快照后 %s ms（快照自身 %s ms）"
          % (ms(block.get("mark_after_pre_stop_al_ns")), ms(block.get("pre_stop_al_read_ns"))))
    print("    审计收尾后 %s ms" % ms(block.get("mark_after_audit_ns")))
    print("    第一条安全停机帧 %s ms" % ms(block.get("mark_first_safe_frame_ns")))


def dump_cycles(report):
    block = report.get("shutdown_cycles") or {}
    if not block:
        print("  停机循环：报告里没有这一段")
        return
    print("  停机循环：共 %s 拍，保头 %s 拍，提前中止（未发帧）=%s"
          % (block.get("cycle_total"), block.get("sample_count"),
             block.get("aborted_before_exchange")))
    if block.get("aborted_before_exchange"):
        print("    中止于轴 %s 阶段 %s（0=控制器步进 1=过程映像更新）"
              % (block.get("aborted_axis"), block.get("aborted_stage")))
    for index, sample in enumerate(block.get("samples") or []):
        axes = sample.get("axes") or []
        arrow = "解码过" if sample.get("axes_decoded") else "沿用上一拍"
        print("    [%02d] 交换=%s wkc=%s 状态码=%s 全轴安全=%s（状态字%s）"
              % (index, sample.get("exchange"), sample.get("wkc"),
                 sample.get("exchange_status"), sample.get("all_axes_safe"), arrow))
        print("         " + " ".join(
            "轴%d %s%s" % (position + 1, axis.get("status_word"),
                           "" if axis.get("state_known") else "(未解码)")
            for position, axis in enumerate(axes)))
    last = block.get("last")
    if last is not None and (block.get("samples") or [])[-1:] != [last]:
        print("    最后一拍：交换=%s wkc=%s 状态码=%s 全轴安全=%s"
              % (last.get("exchange"), last.get("wkc"), last.get("exchange_status"),
                 last.get("all_axes_safe")))
    if block.get("mismatch_al_read"):
        print("    首次 WKC 不符时的 AL 快照（交换=%s，读取自身耗时 %s ms）："
              % (block.get("mismatch_al_exchange"), ms(block.get("mismatch_al_read_ns"))))
        print("      " + " ".join(
            "轴%s state=%s code=0x%04X" % (row.get("axis"), row.get("state"),
                                           row.get("status_code") or 0)
            for row in block.get("mismatch_al") or []))
    else:
        print("    首次 WKC 不符时的 AL 快照：本轮没触发")


def dump_attempts(report):
    block = report.get("shutdown_cycles") or {}
    origin = (report.get("shutdown_prologue") or {}).get("origin_monotonic_ns") or 0
    rows = block.get("attempts") or []
    if not rows:
        print("  停机首拍定位：报告里没有这一段（更早的 schema？）")
        return

    def rel(value):
        if not value or not origin:
            return "无"
        return "%+.3f" % ((value - origin) / 1e6)

    print("  停机首拍定位（相对序言起点；第 0 拍若被整周期跳发则交换号不推进）：")
    for index, row in enumerate(rows):
        print("    [%02d] %s ms 开始，交换前边界 %s，交换后边界 %s，返回于 %s，交换号=%s "
              "状态=%s，修正=%+d ns，相位误差=%+d ns"
              % (index, rel(row.get("begin_ns")), rel(row.get("deadline_before_ns")),
                 rel(row.get("deadline_after_ns")), rel(row.get("end_ns")),
                 row.get("exchange_after"), row.get("exchange_status"),
                 row.get("correction_ns") or 0, row.get("phase_error_ns") or 0))


def dump_observer(report):
    block = report.get("observer_stop") or {}
    if not block:
        print("  观测线程：报告里没有这一段")
        return
    flag = block.get("stop_flag_ns") or 0
    seen = block.get("stop_seen_ns") or 0
    loop_exit = block.get("loop_exit_ns") or 0
    exit_ns = block.get("exit_ns") or 0

    def segment(start, end):
        return ms(end - start) if start and end and end > start else "-"

    print("  观测线程（第 %s 轮、累计 %s 次邮箱读，单次最长 %s ms）："
          % (block.get("stop_iteration"), block.get("read_count"),
             ms(block.get("read_max_ns"))))
    print("    标志落下时相位=%s（相位起点 %s ms 之前）"
          % (PHASE_NAMES.get(block.get("stop_phase"), "?"),
             "-" if not block.get("stop_phase_begin_ns")
             else ms(flag - block.get("stop_phase_begin_ns"))))
    print("    在被看见于=%s，轴 %s" % (SITE_NAMES.get(block.get("stop_site"), "?"),
                                       block.get("stop_axis")))
    print("    标志→看见 %s ms ／ 看见→循环退出 %s ms ／ 退出→线程返回 %s ms"
          % (segment(flag, seen), segment(seen, loop_exit), segment(loop_exit, exit_ns)))
    print("    单轮最长工作耗时（不含睡眠）%s ms" % ms(block.get("iteration_max_ns")))


def dump_threads(report):
    rows = report.get("thread_schedstat") or []
    if not rows:
        print("  线程调度：报告里没有这一张表")
        return
    print("  线程调度（收尾时累计值）：")
    for row in rows:
        print("    tid=%-7s policy=%s prio=%-3s 运行 %s s 排队等待 %s ms 切换 %s 次"
              % (row.get("tid"), row.get("policy"), row.get("priority"),
                 "%.3f" % ((row.get("exec_ns") or 0) / 1e9),
                 "%.3f" % ((row.get("wait_ns") or 0) / 1e6), row.get("switches")))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as handle:
            report = json.load(handle)
        result = report.get("result", {})
        axes = report.get("axes") or []
        print("=" * 78)
        print("%s  schema=%s" % (path, report.get("schema_version")))
        print("  终态：status=%s 安全输出已发=%s 安全状态到达=%s join=%s ms gap=%s ms"
              % (result.get("status_code"), result.get("safe_output_sent"),
                 result.get("safe_state_reached"),
                 ms(result.get("shutdown_observer_join_ns")),
                 ms(result.get("shutdown_prologue_gap_ns"))))
        if axes:
            print("  停机坐标：入口 %s" % " ".join(
                coordinate(index, axis, "shutdown_al_entry")
                for index, axis in enumerate(axes)))
            print("            停机前 %s" % " ".join(
                coordinate(index, axis, "shutdown_al_pre_stop")
                for index, axis in enumerate(axes)))
            print("            停机后 %s" % " ".join(
                coordinate(index, axis, "shutdown_al")
                for index, axis in enumerate(axes)))
            print("            首帧不符 %s" % " ".join(
                first_mismatch(index, axis) for index, axis in enumerate(axes)))
        dump_prologue(report)
        dump_cycles(report)
        dump_attempts(report)
        dump_observer(report)
        dump_threads(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
