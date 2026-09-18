#!/usr/bin/env python3
"""验 tools/gui_evidence.py：它判得对、也判得出来"没成"。

**为什么值得一件。** 这份判定的价值全在"它会不会红"。上一轮台架就是这个形状：
控制侧的证据（实到角度、发了几条目标、错误码）样样正常，唯独监控半边一帧都没
收到——而脚本把那句"观测：一帧都没收到"原样打印之后就算跑完了。**打印事实的地方
不负责说这算不算数**，于是那一场在纸面上是通过的。

所以这个检查一半是拿**真日志**（那一轮台架原封不动下载下来的两份）当夹具：
  bench_obs_zero_frames.txt   只读臂，零帧  → 必须判红
  bench_drive_zero_frames.txt 接管臂，零帧，但控制侧一切正常 → 必须判红
这两份就是"红"的基准。健康版（healthy_*.txt）是在真日志上改**那一行**得到的：
形状还是现场那份，只有判据变了——所以绿的那几项证明的不是"解析器能解析自己造的东西"。

跑法：python scripts/checks/gui_evidence_check.py
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, "gui_evidence_fixtures")
TOOL = os.path.join(ROOT, "tools", "gui_evidence.py")


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


def run(*args):
    """跑一次工具，返回 (退出码, stdout, stderr)。"""
    result = subprocess.run(
        [sys.executable, TOOL, *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    return result.returncode, result.stdout, result.stderr


def fixture(name):
    return os.path.join(FIX, name)


def check_zero_frames_obs_is_red(context):
    """真日志：只读臂一帧都没收到 → 判红。"""
    code, out, err = run("--expect", "obs", fixture("bench_obs_zero_frames.txt"))
    want(code == 1, f"零帧该判红（退出码 1），实际 {code}；out={out!r}")
    want("一帧都没收到" in out, f"红是红了，但没说清哪条不成立：{out!r}")


def check_zero_frames_drive_is_red(context):
    """真日志：接管臂控制侧全对（30° 走到位、错误码 0），但零帧 → 照样判红。

    **这一份是这次改动的全部理由。** 拿它去跑健康版的判据，前四条都会绿；
    只有"收到帧了没有"这一条会红——而正是这一条，上一轮没人判。
    """
    code, out, err = run("--expect", "drive", fixture("bench_drive_zero_frames.txt"))
    want(code == 1, f"零帧该判红（退出码 1），实际 {code}；out={out!r}")
    want("一帧都没收到" in out, f"没说清哪条不成立：{out!r}")
    # 控制侧那几条必须**仍然判绿**：不是"因为零帧所以整份都不算"，而是"只这一条不成立"。
    want("ok    每根轴都真的走到过" in out,
         f"控制侧那些成立的项目不该跟着一起红：{out!r}")


def check_healthy_logs_are_green(context):
    """健康版：两条臂都该绿——否则这个工具只会红，等于没判。"""
    code, out, err = run("--expect", "obs", fixture("healthy_obs.txt"))
    want(code == 0, f"健康的只读臂该判绿，实际退出码 {code}；out={out!r} err={err!r}")
    code, out, err = run("--expect", "drive", fixture("healthy_drive.txt"))
    want(code == 0, f"健康的接管臂该判绿，实际退出码 {code}；out={out!r} err={err!r}")
    want("59976" in out, f"收到帧的数该报出来：{out!r}")


def check_readonly_arm_must_not_take_over(context):
    """只读臂拿到了命令端点 → 判红。"只读"是纪律，得有人盯着。

    **判的是端点，不是「接管中」。** 收尾报告是在命令通道按设计释放之后打的，
    所以"接管中"在接管臂的报告里也是"否"——拿它判只读，这条判据对**任何**跑完的
    臂都成立，等于没判。第一版就是这么写的，而这一项检查当场把它抓出来了。
    """
    code, out, err = run("--expect", "obs", fixture("healthy_drive.txt"))
    want(code == 1, f"接管过的臂当只读臂判该红，实际 {code}；out={out!r}")
    want("没有拿到命令端点" in out, f"该点名命令端点这条：{out!r}")
    # 那一份日志里「接管中：否」是成立的——恰恰说明只靠它判不出来。
    want("ok    收尾时仍未接管" in out,
         f"这一份的「接管中」本来就该是绿的（它确实是收尾时拍的）：{out!r}")


def check_min_peak_has_teeth(context):
    """把门槛抬到实测极值之上 → 判红。证明"走到位了没有"这一条真的在看数。"""
    code, out, err = run("--expect", "drive", "--min-peak-deg", "31",
                         fixture("healthy_drive.txt"))
    want(code == 1, f"门槛 31° 时（实测最低曾到 30.138°）该判红，实际 {code}；out={out!r}")
    want("31" in out, f"该把门槛说清楚：{out!r}")


def check_truncated_and_missing_block_are_unusable(context):
    """半段/没有自检块：退出码 2（日志不可用），**不是** 1（判据不成立）。

    这个区分是有用的：1 的意思是"界面跑完了、但监控没成"，2 的意思是"这份日志
    根本没跑到收尾"——要去查的地方完全不同（一个是主站/桥，一个是界面本身）。
    """
    code, out, err = run("--expect", "args-not-used", fixture("truncated.txt"))
    want(code == 2, f"用法错该是 2，实际 {code}")
    code, out, err = run("--expect", "obs", fixture("truncated.txt"))
    want(code == 2, f"半段日志该判「不可用」（2），实际 {code}；out={out!r}")
    want("自检结束" in err, f"该说清缺的是哪半边：{err!r}")
    code, out, err = run("--expect", "obs", fixture("no_block.txt"))
    want(code == 2, f"没有自检块该判「不可用」（2），实际 {code}；out={out!r}")


CHECKS = [
    ("真日志·只读臂零帧 → 红", check_zero_frames_obs_is_red),
    ("真日志·接管臂零帧（控制侧全对）→ 红", check_zero_frames_drive_is_red),
    ("健康版两条臂 → 绿", check_healthy_logs_are_green),
    ("只读臂接管了 → 红", check_readonly_arm_must_not_take_over),
    ("行程门槛有牙齿（抬到 31° → 红）", check_min_peak_has_teeth),
    ("半段/没有自检块 → 不可用（2），不是不成立（1）", check_truncated_and_missing_block_are_unusable),
]

NOT_COVERED = [
    "健康版的夹具是**合成的**：真日志里那一行是「一帧都没收到」，健康的那行是在真日志上"
    "改出来的。第一份真的健康日志要等下一次台架（主站开了观测开关那一轮）才拿得到。",
    "日志形状：解析器认的是界面现在这几行字。界面改了报告的措辞，这里不会自己知道——"
    "所以台架脚本那边必须**看退出码**，不能只看它打印了什么。",
    "帧的质量：只判「收到没收到」和跳号计数，没判帧里的 pos/vel/torque 合不合理"
    "（那是曲线模块的事，且只能人眼看）。",
]


def main():
    failures = []
    for name, function in CHECKS:
        try:
            function({})
        except Exception as error:  # noqa: BLE001
            print(f"FAIL  {name}")
            print(f"      {type(error).__name__}: {error}")
            failures.append(name)
        else:
            print(f"ok    {name}")

    print()
    print(f"{len(CHECKS) - len(failures)}/{len(CHECKS)} 项通过")
    if failures:
        print("失败：")
        for name in failures:
            print(f"  - {name}")
    print()
    print("没证到的：")
    for note in NOT_COVERED:
        print(f"  - {note}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
