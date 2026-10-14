#!/usr/bin/env python3
"""台架客户端「只动一根轴」的语义不许退化，离线验。

    python3 tests/checks/single_axis_client_check.py
    python3 tests/checks/single_axis_client_check.py --mutate   # 证明这检查抓得住退化

为什么值得单独验（P12.40 的排查手段）：八轴标准轮里第 8 根轴全程没动，可整轮报的是
"干净"——要分清"它自己的问题"还是"八根一起时的别的问题"，最直接的办法是**只命令那一根**。
但主站的 SET_EXTERNAL_TARGET 分支按 `count == axis_count` 精确核对（session_control.c），
少发几个数会被整条拒掉，所以"只动一根"只能靠把其余轴的目标**钉在起点**实现：它们照常每拍
收到目标，只是那条目标等于原地保持。

这里判三件事，缺一条这个排查手段就不能用：

  1. `--only N`：第 N 轴扫满位移，其余轴**全程一动不动**（不是"少发"，是钉住）；
  2. `--only 0`：全部轴一起走——这是此前所有台架轮次的语义，不许被改掉；
  3. `--only` 超出轴数必须**报错退出**。这条最要紧：若它悄悄退化成"全部轴都走"，
     排查那一轮会看起来完全正常，得出的结论正好是反的。

变异对照在**副本**上做（绝不改仓库里的文件）：把只动一根轴改成忽略 `--only`、全部轴都走，
检查必须变红。

只用 `--dry-run`，不连主站、不碰硬件。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLIENT = os.path.join(ROOT, "tools", "bench", "test_external_motion_client.py")

FRAME = re.compile(r"目标=\[([^\]]*)\]")
DISPLACEMENT = re.compile(r"位移=(\d+) counts")

# 变异：把"只动第 N 轴"改成"所有轴都按同一个位移走"——正好是"第 N 轴没单独拎出来"的写法。
MUTATE_FROM = ("    deltas = [0] * axis_count\n    deltas[only - 1] = delta\n")
MUTATE_TO = "    deltas = [delta] * axis_count\n"


def run_client(client_path, args):
    """跑一次 --dry-run，返回 (退出码, stdout+stderr)。"""
    environment = dict(os.environ)
    environment["PYTHONIOENCODING"] = "utf-8"
    result = subprocess.run(
        [sys.executable, client_path, "--dry-run"] + args,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", env=environment)
    return result.returncode, result.stdout


def frames(output):
    """把打印出来的每一条目标行解析成整数列表。"""
    parsed = []
    for line in output.splitlines():
        match = FRAME.search(line)
        if match:
            parsed.append([int(value) for value in match.group(1).split(",")])
    return parsed


def spans(parsed):
    """每轴单独看：整段里最大值与最小值之差，即这根轴实际走了多少。"""
    columns = list(zip(*parsed))
    return [max(column) - min(column) for column in columns]


def leg(output):
    """客户端自己报的本次位移（counts）。期望值取自它，不在检查里另写一个数——
    否则换算比例一改（编码器/减速比），检查要么假红要么永远成立。"""
    match = DISPLACEMENT.search(output)
    if match is None:
        raise ValueError("输出里没有 位移=... counts 那一行")
    return int(match.group(1))


def spans_are(parsed, expected):
    """逐轴比对位移。expected 用 None 表示"这根轴就该是一动不动的 0"。"""
    if not parsed:
        return False, "没有解析到任何目标行"
    actual = spans(parsed)
    if len(actual) != len(expected):
        return False, f"轴数对不上：期望 {len(expected)}，实际 {len(actual)}"
    for index, (got, want) in enumerate(zip(actual, expected), 1):
        if want is None:
            if got != 0:
                return False, f"第 {index} 轴本应一动不动，实际走了 {got} counts"
        elif got != want:
            return False, f"第 {index} 轴走了 {got} counts，期望 {want}"
    return True, f"逐轴位移 = {actual}"


def main():
    mutate = "--mutate" in sys.argv[1:]
    failures = []

    def check(name, ok, detail):
        print(f"[{'OK' if ok else '红'}] {name}：{detail}")
        if not ok:
            failures.append(name)

    # --- 1. 只动第 8 轴，其余钉住 ---
    code, output = run_client(CLIENT, ["--dry-run-axes", "8",
                                       "--degrees", "30", "--traverse", "5", "--rate", "100",
                                       "--only", "8"])
    parsed, wanted = frames(output), leg(output)
    ok, detail = spans_are(parsed, [None] * 7 + [wanted])
    check("--only 8：只有第 8 轴走", code == 0 and ok,
          f"退出码={code} 位移={wanted} counts {detail}")

    # --- 2. --only 0 仍是全部轴一起走 ---
    tail = ["--degrees", "30", "--traverse", "5", "--rate", "100"]
    code, output = run_client(CLIENT, ["--dry-run-axes", "3"] + tail + ["--only", "0"])
    parsed, wanted = frames(output), leg(output)
    ok, detail = spans_are(parsed, [wanted] * 3)
    check("--only 0：全部轴一起走", code == 0 and ok,
          f"退出码={code} 位移={wanted} counts {detail}")

    # --- 3. 超出轴数必须报错，不许退化成"全部轴都走" ---
    code, output = run_client(CLIENT, ["--dry-run-axes", "3", "--only", "4", "--degrees", "30"])
    check("--only 超出轴数要报错", code != 0 and "超出轴数" in output,
          f"退出码={code}（期望非 0），输出末行：{output.strip().splitlines()[-1] if output.strip() else '空'}")

    # --- 变异对照：在副本上把"只动一根"改回"全部都动" ---
    if mutate:
        with open(CLIENT, encoding="utf-8") as handle:
            source = handle.read()
        if MUTATE_FROM not in source:
            # 找不到要改的那段说明这检查的对照已经失效——不能当通过。
            check("变异对照", False, "副本里找不到要变异的那段，对照无效（检查本身要跟着改）")
        else:
            directory = tempfile.mkdtemp(prefix="single-axis-mutate-")
            try:
                mutated = os.path.join(directory, os.path.basename(CLIENT))
                with open(mutated, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(source.replace(MUTATE_FROM, MUTATE_TO))
                code, output = run_client(mutated, ["--dry-run-axes", "8", "--only", "8",
                                                    "--degrees", "30", "--traverse", "5",
                                                    "--rate", "100"])
                try:
                    parsed, wanted = frames(output), leg(output)
                    caught, detail = spans_are(parsed, [None] * 7 + [wanted])
                    caught = not caught
                except (IndexError, ValueError) as error:
                    caught, detail = True, f"变异副本读不出结果（{error}），按抓住算"
                check("变异对照（副本：只动一根 → 全部都动）", caught,
                      "检查变红（符合预期）" if caught else f"检查没抓住——这条规则是假的（{detail}）")
            finally:
                shutil.rmtree(directory, ignore_errors=True)
    else:
        print("[跳过] 变异对照（加 --mutate）")

    if failures:
        print(f"\n结论：{len(failures)} 项没过 —— {', '.join(failures)}")
        return 1
    print("\n结论：全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
