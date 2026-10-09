#!/usr/bin/env python3
"""整个系列的判据：把 reports/_<SERIES>/index.tsv 读成一句话 + 一个退出码。

用法：
    python3 analysis/series_verdict.py reports/_duodec25b [--strict]

为什么要有它。index.tsv 已经把一轮蒸馏成一行 28 列，但那只是**数据**，不是**判定**：
"这轮到底行不行"要读的人自己在脑子里过一遍。四组系列跑下来，人眼阅卷错了两次 ——
duodec25b 报 5/5（实际 4/5）、duodec30 的轴 12 一步没动而五轮全记成干净。
判定落在纸上、并且给得出退出码，后面的对照实验才能连做。

判据分两档，写在这里而不是散在文档里：

  FAIL（让整个系列判不过）
    · exit_code != 0    —— 以每轮自己写的 exit-code.txt 为准。index.tsv 那一列曾经
                            恒为 0（单轮脚本的 finish() 只 echo 不 exit，已修 35bc4e6），
                            所以旧系列只能从 exit-code.txt 取真值，回退到 index 会漏判。
    · shutdown_in_op 不是 N/N —— 收尾有轴不在 OP 就是掉了轴。
    · 任一轴 following_error_max >= 该轴位移的一半 —— **死轴**。一步没动的轴，
                            跟随误差正好等于整条位移；而多轴档护栏是 50°、命令 30°，
                            它不会触发护栏，整轮照样以退出码 0 结束（duodec30 实测）。
    · op_reached / all_enabled 不为 1。

  WARN（列出来，不翻转判定；--strict 下也算不过）
    · wkc_expected != 3 × 轴数 —— 帧构成变了。观察过 8 轴时出现 24 与 30 两种期望值，
                            期望值本身漂移会让 WKC 判据整轮偏松或偏紧（P12.53）。
    · wkc_mismatch / wkc_no_frame > 0、deadline_missed / over_budget > 0。

不复用为判据的量，写在这里免得下一个人再去试：

  · motion_completed —— 四组系列里不论跑得多干净它都是 0。恒为假就区分不出任何东西，
                        当判据只会让每轮都红。它本身是个待查项（报告的"运动完成"
                        为什么不置真），记在 WARN 里但不参与判定。
  · frame_interval_max / receive_max / errctr_max —— 极值，轮轮都不同（1015–1104 µs、
                        180–268 µs、118–210 µs），没有阈值依据，只印出来供人看趋势。

退出码：0 = 无 FAIL；1 = 有 FAIL；2 = 用法或读文件出错（与 series_index.py 同口径）。
"""

import argparse
import glob
import json
import os
import sys

# 死轴判据的阈值：跟随误差达到该轴位移的这个比例就判死。
# 留这么大余量是因为健康轴实测 767–1453 counts（28:1 下约 0.6–1.1°），而一步没动的轴
# 正好等于整条位移（38229），中间空着一个数量级，取 0.5 不会误伤。
DEAD_AXIS_RATIO = 0.5

# 帧里每个从站贡献的工作计数器条数。实测 10 轴 30、12 轴 36、8 轴 24，都是 3×轴数。
WKC_PER_SLAVE = 3

EXIT_MEANING = {
    "0": "跑满且命令流连续",
    "1": "命令流出现过 200ms 空档",
    "2": "主站中途中断了命令流",
    "3": "shutdown 没能停住主站（P12.1）",
    "64": "环境不对（ulimit -r）",
}


def repo_root():
    """脚本在 analysis/ 下，仓库根是往上两级。"""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_index(path):
    """读 index.tsv，返回 (header, [row_dict])。第一列是 series，没有行号列。"""
    with open(path, encoding="utf-8") as handle:
        lines = [line.rstrip("\n") for line in handle if line.strip()]
    if len(lines) < 1:
        raise ValueError("index.tsv 是空的")
    header = lines[0].split("\t")
    rows = []
    for line in lines[1:]:
        cells = line.split("\t")
        # 列数不足的补 "-"：宁可少一个数，也不要因为下游改过列数就整份读不了。
        cells += ["-"] * (len(header) - len(cells))
        rows.append(dict(zip(header, cells)))
    return header, rows


def series_dir_of(path):
    """接受系列目录或 index.tsv 本身，统一返回系列目录。"""
    return os.path.dirname(path) if os.path.basename(path) == "index.tsv" else path


def true_exit_code(sdir, round_name, index_value):
    """真退出码。优先读每轮目录里的 exit-code.txt —— 那是判定发生的当场写下的。

    index.tsv 的 exit_code 列取自调用方的 $?，而单轮脚本的 finish() 一度只 echo 不
    exit，于是"跑满/有空洞/被中断/shutdown 没停住"四种结局在那一列里全是 0。
    所以这里把 index 的值只当兜底。
    """
    matches = sorted(glob.glob(os.path.join(sdir, "r*_" + round_name, "exit-code.txt")))
    if not matches:
        return index_value, "index"
    try:
        with open(matches[0], encoding="utf-8") as handle:
            return handle.read().strip(), "exit-code.txt"
    except OSError:
        return index_value, "index"


def resolve_displacement(deployment_id):
    """按 deployment_id 查出每轴的位移（counts），用于判死轴。

    链：部署档 -> motion_profile_id -> 运动方案 -> 每轴 gear 与角度。
    位移 = enc/enc_rev × gear_motor/gear_shaft × 角度(度) / 360。

    查不到就返回 None —— 调用方据此跳过死轴判据并**明说跳过了**，不猜。
    """
    root = repo_root()
    deployment_path = None
    for path in sorted(glob.glob(os.path.join(root, "config", "deployments", "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        if data.get("deployment_id") == deployment_id:
            deployment_path = path
            break
    if deployment_path is None:
        return None

    with open(deployment_path, encoding="utf-8") as handle:
        profile_id = json.load(handle).get("motion_profile_id")
    for path in sorted(glob.glob(os.path.join(root, "config", "motion_profiles", "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                profile = json.load(handle)
        except (OSError, ValueError):
            continue
        if profile.get("motion_profile_id") != profile_id:
            continue
        displacements = []
        for axis in profile.get("axes", []):
            scale = axis.get("expected_position_scale", {})
            increments = float(scale.get("encoder_increments", 0) or 0)
            enc_revs = float(scale.get("encoder_motor_revolutions", 1) or 1)
            gear_motor = float(scale.get("gear_motor_revolutions", 0) or 0)
            gear_shaft = float(scale.get("gear_shaft_revolutions", 1) or 1)
            degrees = float(axis.get("relative_angle_millidegrees", 0) or 0) / 1000.0
            if enc_revs <= 0 or gear_shaft <= 0:
                return None
            displacements.append(abs(increments / enc_revs * gear_motor / gear_shaft * degrees / 360.0))
        return displacements or None
    return None


def resolve_wkc_policy(deployment_id):
    """按 deployment -> error_recovery_policy_id 查策略，返回两类的阈值。

    返回 {"short": (连续, 窗口次数, 窗口毫秒), "no_frame": 同}；查不到返回 None。

    阈值从配置读而不是写死：这些数就是判定边界，写死在工具里之后，改配置不会改判定，
    而"改了配置判定没跟着变"是最难发现的一类不一致。
    """
    root = repo_root()
    policy_id = None
    for path in sorted(glob.glob(os.path.join(root, "config", "deployments", "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        if data.get("deployment_id") == deployment_id:
            policy_id = data.get("error_recovery_policy_id")
            break
    if not policy_id:
        return None

    policy_path = os.path.join(root, "config", "error_recovery_policies", policy_id + ".json")
    try:
        with open(policy_path, encoding="utf-8") as handle:
            policy = json.load(handle)
    except (OSError, ValueError):
        return None

    def triple(section):
        block = policy.get(section) or {}
        return (block.get("consecutive_error_threshold"),
                block.get("total_error_threshold"),
                block.get("total_error_window_ms"))

    return {"short": triple("wkc_recovery"), "no_frame": triple("no_frame_recovery"),
            "policy_id": policy_id}


def number(text, default=0):
    """index 里读不到的字段一律是 "-"，它不等于 0，但判据里要当"没有这一项"处理。"""
    try:
        return int(str(text).strip())
    except (TypeError, ValueError):
        return default


def judge(row, sdir, displacements, policy):
    """判一轮，返回 (failures, warnings)。两者都是人话字符串的列表。"""
    failures = []
    warnings = []

    code, source = true_exit_code(sdir, row.get("round", ""), row.get("exit_code", "-"))
    if code not in ("0", None) and str(code).strip() != "0":
        meaning = EXIT_MEANING.get(str(code).strip(), "未登记的含义")
        failures.append("exit=%s（%s，取自 %s）" % (code, meaning, source))

    in_op = str(row.get("shutdown_in_op", "-")).strip()
    if "/" in in_op:
        got, _, total = in_op.partition("/")
        if got != total:
            failures.append("收尾在 OP %s，不是 %s" % (in_op, total + "/" + total))
    else:
        warnings.append("shutdown_in_op 读不出来（%r）" % in_op)

    if number(row.get("op_reached"), 0) != 1:
        failures.append("没进 OP（op_reached=%s）" % row.get("op_reached"))
    if number(row.get("all_enabled"), 0) != 1:
        failures.append("有轴没使能（all_enabled=%s）" % row.get("all_enabled"))

    raw_errors = str(row.get("following_error_max", "-")).strip()
    errors = [number(v, -1) for v in raw_errors.split(",")] if raw_errors != "-" else []
    if errors and displacements:
        if len(errors) != len(displacements):
            warnings.append("跟随误差 %d 项、位移 %d 项，对不上，跳过死轴判据"
                            % (len(errors), len(displacements)))
        else:
            for index, (error, span) in enumerate(zip(errors, displacements), start=1):
                if span > 0 and error >= span * DEAD_AXIS_RATIO:
                    note = ""
                    # 位移是按**今天的配置**算的。系列跑完之后若有人改过运动方案
                    # （实测：duodec30 那组跑完才把轴 12 从 28:1 改成 25:1），
                    # 跟随误差与位移就对不上号。判定仍然成立（死轴是"跟随误差等于
                    # 整条位移"，两者差着一个数量级），但数字并排印出来会让人以为
                    # 哪里算错了——所以把这件事说出来，不留给读的人去猜。
                    # 容差取 5%：健康轴与死轴差着一个数量级，5% 足以把「配置改过」
                    # 与「正常波动」分开，又不会因为取整而漏报。
                    if span > 0 and abs(error - span) > span * 0.05:
                        note = "（位移按今天的运动方案算；本轮跑的时候可能不是这个值）"
                    failures.append("轴 %d 一步没动：跟随误差 %d ≈ 位移 %d counts%s"
                                    % (index, error, int(span), note))
    elif not displacements:
        warnings.append("查不到运动方案，跳过死轴判据（跟随误差仍印在下面）")

    axes = None
    if "/" in in_op:
        axes = number(in_op.partition("/")[2], 0) or None
    expected_wkc = number(row.get("wkc_expected"), 0)
    if axes and expected_wkc and expected_wkc != WKC_PER_SLAVE * axes:
        warnings.append("WKC 期望 %d ≠ %d×轴数（%d）——帧构成与从站数对不上（P12.53）"
                        % (expected_wkc, WKC_PER_SLAVE, WKC_PER_SLAVE * axes))

    # WKC 一类：判据是**连续**数，不是累计数。策略里 consecutive_error_threshold 与
    # total_error_threshold（后者是滑动窗口内的次数，不是整轮累计）是两条并联的边。
    # 用累计数比较会把"散落 2 次"与"连续 5 次"看成同一件事——实测里前者跑完、
    # 后者中止，区别只在连续数上。
    for key, label, section in (("wkc_max_consecutive", "短帧连续不符", "short"),
                                ("wkc_no_frame_max_consecutive", "整帧缺失连续", "no_frame")):
        value = number(row.get(key), 0)
        if value <= 0:
            continue
        limit = (policy or {}).get(section, (None, None, None))[0]
        if limit:
            warnings.append("%s 最长 %d 次（阈值 %d，策略 %s）"
                            % (label, value, limit, policy.get("policy_id")))
        else:
            warnings.append("%s 最长 %d 次（策略阈值未知）" % (label, value))

    for key, label in (("wkc_short_window_max", "短帧窗口峰值"),
                       ("wkc_no_frame_window_max", "整帧窗口峰值"),
                       ("deadline_missed", "截止期错失"), ("over_budget", "超预算")):
        value = number(row.get(key), 0)
        if value > 0:
            warnings.append("%s %d" % (label, value))

    # 恒为假就区分不出任何东西，所以只提示、不判。
    if number(row.get("motion_completed"), 1) == 0:
        warnings.append("motion_completed=0（四组系列里恒为假，见文件头）")

    return failures, warnings


def main():
    parser = argparse.ArgumentParser(
        description="读一个系列的 index.tsv，给出每轮判定与总结论")
    parser.add_argument("path", help="系列目录（reports/_<SERIES>）或其中的 index.tsv")
    parser.add_argument("--strict", action="store_true",
                        help="把 WARN 也当不过（默认只有 FAIL 影响退出码）")
    args = parser.parse_args()

    sdir = series_dir_of(args.path)
    index_path = args.path if os.path.basename(args.path) == "index.tsv" \
        else os.path.join(args.path, "index.tsv")
    if not os.path.isfile(index_path):
        print("错误：找不到 %s" % index_path, file=sys.stderr)
        return 2

    try:
        _, rows = load_index(index_path)
    except (OSError, ValueError) as error:
        print("错误：读不了 %s（%s）" % (index_path, error), file=sys.stderr)
        return 2

    displacements = None
    policy = None
    if rows:
        displacements = resolve_displacement(rows[0].get("deployment", ""))
        policy = resolve_wkc_policy(rows[0].get("deployment", ""))

    failed_rounds = 0
    warned_rounds = 0
    for row in rows:
        failures, warnings = judge(row, sdir, displacements, policy)
        verdict = "FAIL" if failures else ("WARN" if warnings else "PASS")
        if failures:
            failed_rounds += 1
        elif warnings:
            warned_rounds += 1
        print("%-14s %-4s cycles=%-7s wkc=%s/%s 收尾=%s" % (
            row.get("round", "?"), verdict, row.get("cycles", "-"),
            row.get("wkc_actual", "-"), row.get("wkc_expected", "-"),
            row.get("shutdown_in_op", "-")))
        for line in failures:
            print("    FAIL %s" % line)
        for line in warnings:
            print("    warn %s" % line)
        if failures:
            not_in_op = str(row.get("not_in_op", "-")).strip()
            if not_in_op != "-":
                print("    不在 OP：%s" % not_in_op)

    total = len(rows)
    passed = total - failed_rounds
    print()
    if displacements is None:
        print("注意：查不到该部署的运动方案，死轴判据本轮**没有执行**")
    print("%s: %d/%d 通过（%d FAIL，%d 仅有警告）" % (
        os.path.basename(sdir.rstrip("/")), passed, total, failed_rounds, warned_rounds))

    if failed_rounds:
        return 1
    if warned_rounds and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
