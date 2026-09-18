#!/usr/bin/env python3
"""读界面的无头自检日志，判"这一臂的监控/控制到底成没成"——判 **成** 还是 **没成**。

**为什么要有它。** 2026-09-18 那次台架，四个臂全绿、A/B 三项全 0、五轴 30° 往返
每一趟都走到了——而**监控半边一帧都没收到**。日志上那句话是"观测：一帧都没收到"，
它躺在几百行输出里，台架脚本把它原样 tee 出来就算完了。控制侧的证据（实到角度、
发了几条目标）看着完全正常，于是这一场"端到端控制 + 监控验证"在纸面上是通过的。

问题出在"报告"和"判定"混在一起：打印事实的地方不负责说这算不算数。所以这里把
判定单独拎出来，让它有一个**会失败的退出码**，并且能拿几份假日志离线验它。

判什么，按臂说（`--expect`）：
  obs    只读臂：**必须收到帧**，且不许接管命令通道（只读是纪律，不是省事）
  drive  接管臂：必须收到帧、必须接管过、**必须真的动过轴**（到过 +30° 一档）
  bridge 只有桥没有界面：这里没有界面日志可判，用它去判是错的，直接报不支持

"必须真的动过轴"用的是**曾到过**那一列（自检报告里叫"曾到过="）。它和"实际="不一样：
本轮实测轴1 实际=+0.128°、曾到过=+30.192°——回程回到起点之后，只剩"曾到过"还
留着那一趟的证据。拿"实际="判会得出"轴没动"的结论。

跑法：
  python tools/gui_evidence.py --expect drive /tmp/gui_e2e_gui_4_r1_drive.txt
  python tools/gui_evidence.py --expect obs   /tmp/gui_e2e_gui_3_r1_obs.txt
退出码 0 = 判据都成立；1 = 有一条不成立（会打印是哪条）；2 = 用法/日志本身有问题。
"""

import argparse
import re
import sys

BLOCK_BEGIN = "---- GUI 自检 ----"
BLOCK_END = "---- 自检结束 ----"

# 最小行程：往来目标是 30°，这里只要求"到过 20° 以上"，别把判据卡在实测的极值上。
# 台架脚本允许用 RECIP_DEG 改幅度，所以这个数要能被 --min-peak-deg 顶掉。
DEFAULT_MIN_PEAK_DEG = 20.0


class EvidenceError(Exception):
    """日志本身不可用（截断、没有自检块）——不是判据不成立。"""


def split_block(text):
    """抠出 `---- GUI 自检 ----` 到 `---- 自检结束 ----` 那一段。

    缺任一边界都算不可用：自检没跑到收尾（超时被杀、线程挂住）时，块是不完整的，
    而"没有块"和"块里写着一切正常"必须区分开——前者是没证据。
    """
    begin = text.find(BLOCK_BEGIN)
    end = text.find(BLOCK_END)
    if begin < 0:
        raise EvidenceError(f"日志里没有「{BLOCK_BEGIN}」：自检没走到收尾报告")
    if end < 0 or end < begin:
        raise EvidenceError(f"有「{BLOCK_BEGIN}」但没有「{BLOCK_END}」："
                            f"自检被打断了，这份日志只有半段")
    return text[begin:end]


_NUM = r"[-+]?\d+(?:\.\d+)?"


def parse(block):
    """把自检块解析成一份字典。取不到的字段一律是 None（**不是 0**）。"""
    out = {
        "received": None,       # 观测收到几帧
        "gaps": None,
        "resyncs": None,
        "zero_frames_reason": None,
        "takeover": None,       # True/False/None
        "cmd_endpoint": None,
        "sent": None,           # 已发目标条数
        "max_step": None,
        "peak_deg": [],         # 各轴"曾到过"
        "actual_deg": [],
        "error_codes": [],
        "status_words": [],
    }

    match = re.search(rf"观测：收到 (\d+) 帧，跳号 (\d+) 处.*重新对齐 (\d+) 次", block)
    if match:
        out["received"] = int(match.group(1))
        out["gaps"] = int(match.group(2))
        out["resyncs"] = int(match.group(3))
    else:
        match = re.search(r"观测：一帧都没收到(.*)", block)
        if match:
            out["received"] = 0
            out["zero_frames_reason"] = match.group(1).strip() or None

    match = re.search(r"接管中：(是|否)", block)
    if match:
        out["takeover"] = match.group(1) == "是"

    match = re.search(r"命令端点：(.*)", block)
    if match:
        out["cmd_endpoint"] = match.group(1).strip()

    match = re.search(rf"已发 (\d+) 条目标，相邻目标最大增量 (\d+) counts", block)
    if match:
        out["sent"] = int(match.group(1))
        out["max_step"] = int(match.group(2))

    for line in block.splitlines():
        axis = re.search(rf"轴(\d+): 实际=({_NUM})° .*曾到过=({_NUM})° "
                         rf"状态字=(\S+) 错误码=(\S+)", line)
        if axis:
            out["actual_deg"].append(float(axis.group(2)))
            out["peak_deg"].append(float(axis.group(3)))
            out["status_words"].append(axis.group(4))
            out["error_codes"].append(axis.group(5))
    return out


class Verdict:
    def __init__(self):
        self.claims = []          # (成立与否, 说明)

    def claim(self, ok, text):
        self.claims.append((bool(ok), text))

    def failed(self):
        return [text for ok, text in self.claims if not ok]

    def render(self):
        for ok, text in self.claims:
            print(f"{'ok   ' if ok else 'FAIL '} {text}")


def _frames_claim(facts, who):
    """"收到帧了没有"这一条。**这是本轮那次台架唯一没成立的判据。**

    收不到时要把界面的解释带上（"为什么没收到"），但只有真的有那句话时才带——
    写成"日志说：None"既不读起来别扭，也把"没有解释"和"解释是 None"混了。
    """
    received = facts["received"]
    if received:
        return True, (f"{who}收到了观测帧（{received} 帧，跳号 {facts['gaps']} 处，"
                      f"重新对齐 {facts['resyncs']} 次）")
    text = f"{who}一帧都没收到"
    if facts["zero_frames_reason"]:
        text += f"；界面这边最后一句是「{facts['zero_frames_reason']}」"
    return False, text


def judge(expect, facts, min_peak_deg=DEFAULT_MIN_PEAK_DEG):
    verdict = Verdict()

    if expect == "obs":
        verdict.claim(*_frames_claim(facts, "只读臂"))
        # 只读这条**不能只判「接管中：否」**：报告是在收尾时打的，那个字段是**当场**
        # 的状态，接管过又释放掉的臂在报告里也是"否"（接管臂那一份就是"接管中：否"）。
        # 真正决定只读的是**没有命令端点可连**——台架脚本对只读臂就是这么发的，界面
        # 想接管也没得连。所以先判端点，那一条骗不了人。
        verdict.claim(facts["cmd_endpoint"] in (None, "（未给，不会接管）"),
                      "只读臂没有拿到命令端点（这才是它只读的原因）"
                      + f"：{facts['cmd_endpoint']}")
        verdict.claim(facts["takeover"] is False,
                      "收尾时仍未接管命令通道"
                      + ("" if facts["takeover"] is not None
                         else "（读不到「接管中」，日志形状不对）"))
    elif expect == "drive":
        verdict.claim(*_frames_claim(facts, "接管臂"))
        # **不能拿"接管中"判**：自检报告是在收尾时打的，那时命令通道已经按设计释放
        # （现场实测就是"接管中：否"，而这一臂确实接过管、发过 2604 条目标）。
        # 所以判的是"给过命令端点"，那是接管的前提，也是这行字骗不了的地方。
        verdict.claim(facts["cmd_endpoint"] not in (None, "（未给，不会接管）"),
                      "接管臂拿到了命令端点（接管的前提）"
                      + f"：{facts['cmd_endpoint']}")
        verdict.claim(bool(facts["peak_deg"]),
                      "轴表有快照（否则这一臂没发过目标）")
        if facts["peak_deg"]:
            lowest = min(facts["peak_deg"])
            verdict.claim(lowest >= min_peak_deg,
                          f"每根轴都真的走到过 {min_peak_deg:g}° 以上"
                          f"（最低的一根曾到过 {lowest:+.3f}°，共 {len(facts['peak_deg'])} 轴）")
        # 用"曾到过"而不是"实际"：回程回到起点之后，只剩"曾到过"还留着那一趟的证据。
        # 本轮实测：实际=+0.128°、曾到过=+30.192°——拿"实际"判会得出"轴没动"。
        if facts["error_codes"]:
            bad = sorted({code for code in facts["error_codes"] if code != "0x0000"})
            verdict.claim(not bad,
                          "各轴错误码都是 0x0000" if not bad else f"有轴报错：{bad}")
        else:
            verdict.claim(False, "轴表里没有错误码（日志形状不对，这一项没证到）")
    else:
        raise EvidenceError(f"不认识的臂：{expect}")
    return verdict


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="判一份界面无头自检日志：这一臂的监控/控制到底成没成")
    parser.add_argument("log", help="界面自检的日志文件（无头跑的 stdout+stderr）")
    parser.add_argument("--expect", required=True, choices=("obs", "drive"),
                        help="这一臂是什么臂，决定该有哪些证据")
    parser.add_argument("--min-peak-deg", type=float, default=DEFAULT_MIN_PEAK_DEG,
                        help=f"每根轴至少要走到过多少度（默认 {DEFAULT_MIN_PEAK_DEG:g}，"
                             f"要与台架脚本的 RECIP_DEG 对得上）")
    parser.add_argument("--quiet", action="store_true", help="只出结论，不打逐条判据")
    args = parser.parse_args(argv)

    try:
        with open(args.log, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        facts = parse(split_block(text))
    except (OSError, EvidenceError) as error:
        print(f"** 这份日志判不了：{error}", file=sys.stderr)
        return 2

    verdict = judge(args.expect, facts, args.min_peak_deg)
    if not args.quiet:
        verdict.render()
    failed = verdict.failed()
    if failed:
        print(f"** {args.expect} 臂：监控/控制这一臂的判据有 {len(failed)} 条不成立"
              f"——这一臂不算通过", file=sys.stderr)
        return 1
    print(f"{args.expect} 臂：判据全部成立")
    return 0


if __name__ == "__main__":
    sys.exit(main())
