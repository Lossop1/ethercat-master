#!/usr/bin/env python3
"""分析类工具的退出码检查：**坏输入必须以非零退出**（离线，不接总线）。

用法:
    python3 tests/checks/tools_exit_code_check.py

为什么专门查这一条。2026-10-08 发现过一起：单轮台架脚本的 finish() 只 echo 不 exit，
于是"跑满／有空隙／被主站中断／shutdown 没停住"四种结局在进程退出码上全是 0，
而多轮驱动把这个 $? 写进索引——"收尾掉轴"于是显示成干干净净的一轮。

**判据与输出不一致，比判错更危险**：判错会被人看出来，而退出码恒为 0 会让自动化
一路绿灯。所以这里只查一条性质：**给不存在的输入，工具必须非零退出**。

查的是分析类工具（读文件、出结论的那些）。台架类脚本要有装置才能跑，不在这里。

退出码：0 = 全部符合；1 = 有工具在不存在的输入上返回了 0；2 = 内部错误。
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (工具, 参数) —— 参数一律指向一个不存在的路径，工具必须非零退出。
CASES = (
    ("tools/analysis/report_row.py", ["/nonexistent/report.json", "r01"]),
    ("tools/analysis/series_verdict.py", ["/nonexistent/series"]),
    ("tools/analysis/anomalies.py", ["/nonexistent/master.log"]),
    ("tools/analysis/series_index.py", ["/nonexistent/series"]),
    ("tools/analysis/report_summary.py", ["/nonexistent/report.json"]),
)

# 正例：必须零退出。用来挡住"所有输入都非零退出"这种反向坏法 ——
# 一个永远失败的检查与一个永远通过的检查一样没有用。
GOOD_CASES = (
    ("tools/analysis/report_row.py", ["--header"]),
    ("tests/checks/config_chain_check.py", []),
)


def run(relative, argv):
    path = os.path.join(REPO, relative)
    if not os.path.isfile(path):
        return None
    done = subprocess.run([sys.executable, path] + argv, capture_output=True, text=True,
                          cwd=REPO)
    return done.returncode


def main():
    bad = 0
    checked = 0

    for relative, argv in CASES:
        code = run(relative, argv)
        if code is None:
            print("跳过（文件不存在）：%s" % relative)
            continue
        checked += 1
        if code == 0:
            # 这是唯一要拦的形状：坏输入被判成成功。
            print("不合格：%s 在坏输入上返回 0（应非零）" % relative)
            bad += 1
        else:
            print("ok   %s 坏输入返回 %d" % (relative, code))

    for relative, argv in GOOD_CASES:
        code = run(relative, argv)
        if code is None:
            print("跳过（文件不存在）：%s" % relative)
            continue
        checked += 1
        if code != 0:
            print("不合格：%s 在正常输入上返回 %d（应 0）" % (relative, code))
            bad += 1
        else:
            print("ok   %s 正常输入返回 0" % relative)

    print("\n检查 %d 项，不合格 %d 项" % (checked, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
