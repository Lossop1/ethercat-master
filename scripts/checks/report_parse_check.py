#!/usr/bin/env python3
"""归档报告的可解析性检查（离线，不接总线）。

用法:
    python3 scripts/checks/report_parse_check.py                 # 便宜检查：每个文件的首尾
    python3 scripts/checks/report_parse_check.py --full          # 完整解析（慢，几百 MB 一个）
    python3 scripts/checks/report_parse_check.py --limit 10      # 只查最近 10 个

为什么分两档。报告是**停机时一次写盘**的，所以现实中的坏法不是"内容错"，而是
"写到一半进程没了" —— 文件被截断。截断的判据很便宜：合法 JSON 的首字符是 { 、末字符是 }，
读文件两头各一个字节就能判，不需要把几百 MB 读进内存。完整解析只在需要时做。

这一档判不出"内容对但结构坏"（例如括号数错了一个），那种要 --full。两档都写明，
免得把便宜检查的结果当成完整结论。

退出码：0 = 全部可解析；1 = 有截断或解析失败；2 = 用法或目录不存在。
"""

import argparse
import glob
import json
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIR = os.path.join(REPO, "runtime", "reports", "archive")
LIVE_REPORTS = os.path.join(REPO, "runtime", "reports")


def cheap_check(path):
    """只读首尾两个字节。返回 None 表示通过，否则返回原因。"""
    try:
        size = os.path.getsize(path)
        if size == 0:
            return "文件是空的"
        with open(path, "rb") as handle:
            head = handle.read(1)
            handle.seek(max(0, size - 4096))
            tail = handle.read()
        if head != b"{":
            return "首字符不是 {（实际 %r）" % head
        stripped = tail.rstrip(b" \t\r\n")
        if not stripped.endswith(b"}"):
            return "末尾不是 }（最后 40 字节 %r）—— 多半是写到一半被中断" % tail[-40:]
        return None
    except OSError as error:
        return "读不了：%s" % error


def full_check(path):
    try:
        with open(path, encoding="utf-8") as handle:
            json.load(handle)
        return None
    except (OSError, ValueError) as error:
        return "解析失败：%s" % str(error)[:160]


def main():
    parser = argparse.ArgumentParser(description="归档报告可解析性")
    parser.add_argument("--dir", default=DEFAULT_DIR)
    parser.add_argument("--full", action="store_true", help="完整解析（慢）")
    parser.add_argument("--limit", type=int, default=0, help="只查最近 N 个（0=全部）")
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        print("错误：目录不存在 %s" % args.dir, file=sys.stderr)
        return 2

    files = sorted(glob.glob(os.path.join(args.dir, "*.json")),
                   key=os.path.getmtime, reverse=True)
    if args.limit:
        files = files[:args.limit]
    # 当前那份（每轮被覆盖写）也要查：它正是"进程刚被中断"最可能留下的残骸。
    live = sorted(glob.glob(os.path.join(LIVE_REPORTS, "*.json")))
    files = live + files

    if not files:
        print("没有找到任何报告文件")
        return 0

    bad = []
    started = time.time()
    total_bytes = 0
    for path in files:
        total_bytes += os.path.getsize(path)
        reason = full_check(path) if args.full else cheap_check(path)
        if reason:
            bad.append((path, reason))

    elapsed = time.time() - started
    print("检查 %d 个报告，共 %.1f MB，用时 %.1f 秒（%s）"
          % (len(files), total_bytes / 1e6, elapsed, "完整解析" if args.full else "首尾检查"))
    for path, reason in bad:
        print("  不可解析：%s\n    %s" % (os.path.relpath(path, REPO), reason))
    if bad:
        return 1
    print("全部可解析")
    if not args.full:
        print("注意：这一档只判首尾，判不出\"首尾完整但中间结构坏\"。需要时加 --full。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
