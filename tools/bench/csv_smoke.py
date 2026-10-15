#!/usr/bin/env python3
"""CSV（CiA402 模式 9）常值速度冒烟：只走命令套接字。

流向与最终部署一致：本脚本 -> /tmp/emaster-<部署>.sock -> 外部目标缓冲区 ->
position_target_source -> 60FF target_velocity。不绕过命令协议，也不碰主站内部状态。

**为什么只发一个恒定速度**：速度模式没有位置环，驱动器不知道轴在哪、也不会把它拉
回来。往复或斜坡这类波形在"跟不跟得上"还不知道的时候先别发——要证明的是这条通道能
把轴按一个速度稳住，不是能画任意曲线。一个值，看它跟不跟。

命令口上的客户机（Link / parse_status / axis_int）直接从 cst_smoke 取：同一件事不该
抄两遍，抄两遍就会各修各的。

流程（每一步都把现场打出来）：
  1. 取 --velocity-limit 当自己的上限。**主站不查速度大小**，所以这一次冒烟的上限
     只能由操作者在命令行当场声明，不给就不跑。
  2. 等 state=4，读一轮基线：每轴的 mode（6061）、vel（606C）、pos（6064）。
  3. 零速段：全程按 --rate 发全 0，同时监控。
  4. 给速度：只给 --axis 那一根发 --velocity（counts/s），保持 --hold 秒。
  5. 立刻回 0，再监控 --zero-back 秒确认回零。
  6. 打印采样汇总，并提示立刻把主站停掉（本脚本不代劳）。

单位：60FF/606C 是**电机侧 counts/s**（编码器 16384 counts/转），不是出轴角度。
换算来自设备 ESI，不在本脚本里猜——要角度自己按部署的设备档算。

用法:
  python3 tools/bench/csv_smoke.py <socket_path> --deployment <部署ID> --velocity-limit V [选项]

选项:
  --axis N        给速度的轴号（总线序号，从 1 起；默认 1）
  --velocity V   速度目标，counts/s（默认 6000，约出轴 4.7°/s）
  --velocity-limit V  本次冒烟允许的最大速度，counts/s，必填
  --hold S       给速度保持时长（默认 5.0）
  --zero S       给速度前的零速段时长（默认 3.0）
  --zero-back S  回零后的确认时长（默认 2.0）
  --rate HZ      目标下发频率（默认 50，必须快于 200ms 看门狗）
  --monitor S    读 status 的间隔（默认 0.5）
  --ready S      等待 RUNNING 的超时（默认 60）
  --dry-run      不连主站，只读配置、核对参数并打印计划
"""

import argparse
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))

from cst_smoke import Link, axis_int, parse_status  # noqa: E402
from emaster_console import (  # noqa: E402
    STATUS_MASK,
    STATUS_OPERATION_ENABLED,
    deployment_selected_mode,
    find_config_document,
)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# 速度模式下驱动器自报的模式显示（6061）。不是 9 就说明模式没切过去，
# 这时发 60FF 是被驱动器忽略的——看起来"没反应"，其实是根本没进去。
VELOCITY_MODE_DISPLAY = 9

# RxPDO 里能随时扔掉的目标：全是 0。三种模式下都是"什么都不做"。
ZERO_FRAME = 0


class Report:
    """采样与打印。表打到 stdout 供现场看，汇总留给证据。"""

    def __init__(self):
        self.rows = []
        self.rejected = 0
        self.min_position = None
        self.max_position = None

    def sample(self, phase, elapsed, sent, axes, axis_index):
        fields = axes[axis_index] if axis_index < len(axes) else {}
        velocity = axis_int(fields, "vel")
        position = axis_int(fields, "pos")
        mode = axis_int(fields, "mode")
        state = axis_int(fields, "state")
        if position is not None:
            self.min_position = position if self.min_position is None else min(
                self.min_position, position)
            self.max_position = position if self.max_position is None else max(
                self.max_position, position)
        self.rows.append((phase, elapsed, sent, velocity, position, mode, state))
        return velocity, position, mode, state

    def print_row(self, phase, elapsed, sent, velocity, position, mode, state, extra=""):
        print(f"[{phase}] t={elapsed:5.2f}s 目标={sent:+6d} "
              f"读回vel={velocity if velocity is not None else '--':>8} "
              f"pos={position if position is not None else '--':>10} "
              f"mode={mode if mode is not None else '--'} "
              f"state={state if state is not None else '--'}{extra}", flush=True)

    def summary(self, axis_index, velocity):
        print("\n—— 采样汇总 ——")
        phases = {}
        for phase, _, sent, read_velocity, position, _, _ in self.rows:
            entry = phases.setdefault(phase, {"n": 0, "vel": [], "pos": [], "sent": set()})
            entry["n"] += 1
            entry["sent"].add(sent)
            if read_velocity is not None:
                entry["vel"].append(read_velocity)
            if position is not None:
                entry["pos"].append(position)
        for phase in ("zero", "drive", "zero-back"):
            entry = phases.get(phase)
            if not entry:
                continue
            moved = (max(entry["pos"]) - min(entry["pos"])) if entry["pos"] else 0
            read = entry["vel"]
            print(f"  {phase:9s} 采样 {entry['n']:3d} 条，"
                  f"下发目标 {sorted(entry['sent'])}，"
                  f"读回 |vel| 最大 {max((abs(v) for v in read), default=0)} counts/s，"
                  f"位置跨度 {moved} counts")
        print(f"  轴 {axis_index + 1}：位置范围 {self.min_position}..{self.max_position} counts")
        print(f"  目标被拒 {self.rejected} 条")


def load_plan(args):
    """从部署配置里取模式。上限由命令行给，理由同 cst_smoke：主站不查速度大小，
    没有任何一层会替这次冒烟把关，上限只能由操作者当场声明。"""
    repo_root = pathlib.Path(args.repo).resolve() if args.repo else REPO_ROOT
    deployment = find_config_document(
        str(repo_root), "deployments", "deployment_id", args.deployment)
    if deployment is None:
        raise SystemExit(f"找不到部署 {args.deployment}（{repo_root}/config/deployments/）")
    mode = deployment_selected_mode(str(repo_root), args.deployment)
    if mode != "csv":
        raise SystemExit(
            f"这个部署选的是 {mode or '(读不出模式)'} 模式，不是 csv。"
            f"本脚本只发速度，模式不对就别跑（CSP 下 60FF 根本不被驱动器读）。")
    limit = args.velocity_limit
    if not isinstance(limit, int) or isinstance(limit, bool) or limit <= 0:
        raise SystemExit(
            f"--velocity-limit 必须是正整数 counts/s（读到 {limit!r}）。"
            f"主站不查速度大小，速度冒烟拒绝在没有当场声明的上限时运行。")
    return mode, limit


def main(argv=None):
    parser = argparse.ArgumentParser(description="CSV 常值速度冒烟（只走命令套接字）")
    parser.add_argument("socket", nargs="?", help="命令套接字路径")
    parser.add_argument("--deployment", help="部署 ID（读模式与上限）")
    parser.add_argument("--repo", default=None,
                        help="部署树/仓库根，找 config/deployments；缺省是本脚本所在的仓库。"
                             "在部署树（主站 --config-dir 指向的那棵）上跑时要显式给")
    parser.add_argument("--axis", type=int, default=1, help="给速度的轴号（从 1 起）")
    parser.add_argument("--velocity", type=int, default=6000, help="速度目标 counts/s")
    parser.add_argument("--velocity-limit", dest="velocity_limit", type=int, default=None,
                        help="本次冒烟允许的最大速度 counts/s，必填。"
                             "主站不查速度大小，这个上限只能由操作者当场声明")
    parser.add_argument("--hold", type=float, default=5.0, help="给速度保持时长（秒）")
    parser.add_argument("--zero", type=float, default=3.0, help="零速段时长（秒）")
    parser.add_argument("--zero-back", dest="zero_back", type=float, default=2.0,
                        help="回零确认时长（秒）")
    parser.add_argument("--rate", type=float, default=50.0, help="目标下发频率（Hz）")
    parser.add_argument("--monitor", type=float, default=0.5, help="读 status 的间隔（秒）")
    parser.add_argument("--ready", type=float, default=60.0, help="等待 RUNNING 的超时（秒）")
    parser.add_argument("--dry-run", action="store_true", help="只核参数、不连主站")
    args = parser.parse_args(argv)

    if not args.deployment:
        raise SystemExit("缺少 --deployment（部署 ID 是模式的唯一来源）")
    if args.rate <= 0.0 or args.monitor <= 0.0:
        raise SystemExit("--rate 和 --monitor 必须是正数")

    mode, limit = load_plan(args)
    print(f"部署 {args.deployment}：模式 {mode}，本次速度上限 ±{limit} counts/s（--velocity-limit）")
    if abs(args.velocity) > limit:
        raise SystemExit(f"--velocity {args.velocity} 超过当场声明的上限 {limit}。")

    if args.dry_run:
        print(f"计划：零速段 {args.zero:g}s → 轴{args.axis} 发 {args.velocity:+d} counts/s "
              f"保持 {args.hold:g}s → 回 0 确认 {args.zero_back:g}s，"
              f"全程 {args.rate:g} Hz，每 {args.monitor:g}s 读一次 status")
        return 0

    if not args.socket:
        raise SystemExit("缺少命令套接字路径（或用 --dry-run 只核参数）")
    if not hasattr(socket, "AF_UNIX"):
        raise SystemExit("本机 Python 没有 AF_UNIX，本脚本只能在香橙派上跑")

    axis_index = args.axis - 1
    link = Link(args.socket)
    report = Report()
    link.connect(args.ready)
    print(f"已连接 {args.socket}")

    axis_count = 0

    def frame(value):
        """整帧：所有轴 0，只有选中的那根给 value。"""
        if axis_count <= 0:
            return []
        values = [ZERO_FRAME] * axis_count
        if axis_index < axis_count:
            values[axis_index] = value
        return values

    def hold(phase, seconds, sent, elapsed_base):
        """按 --rate 发同一帧，同时按 --monitor 读状态采样。返回是否全程被接受。"""
        deadline = time.monotonic() + seconds
        next_sample = time.monotonic()
        started = time.monotonic()
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            response = link.send_frame(frame(sent))
            if not response.startswith("OK|"):
                report.rejected += 1
                print(f"[{phase}] 目标被拒：{response}", flush=True)
                return False
            if now >= next_sample:
                next_sample = now + args.monitor
                header, axes = link.status()
                if header is None:
                    print(f"[{phase}] status 读不到：{header!r}", flush=True)
                    return False
                velocity, position, mode_display, state = report.sample(
                    phase, elapsed_base + (now - started), sent, axes, axis_index)
                report.print_row(phase, elapsed_base + (now - started), sent,
                                 velocity, position, mode_display, state)
                # 模式对不上就别再给速度了：60FF 会被静默忽略。
                if mode_display is not None and mode_display != VELOCITY_MODE_DISPLAY:
                    print(f"[{phase}] 轴{args.axis} 的模式显示是 {mode_display}，"
                          f"不是 {VELOCITY_MODE_DISPLAY}——停下", flush=True)
                    return False
            time.sleep(max(0.0, 1.0 / args.rate - (time.monotonic() - now)))
        return True

    elapsed = 0.0
    ok = True
    try:
        header, axes = link.status()
        if header is None:
            raise SystemExit("主站没回应 status")
        axis_count = int(header.get("axes", "0"))
        if axis_index >= axis_count:
            raise SystemExit(f"--axis {args.axis} 超出轴数 {axis_count}")
        wait_started = time.monotonic()
        while header.get("state") != "4":
            if time.monotonic() - wait_started > args.ready:
                raise SystemExit(f"等 RUNNING 超时，最后 state={header.get('state')}")
            time.sleep(0.2)
            header, axes = link.status()
        print(f"主站 RUNNING：cycle={header.get('cycle')} 轴数={axis_count}")
        for index, fields in enumerate(axes):
            state_word = axis_int(fields, "status") or 0
            print(f"  轴{index + 1}: mode={axis_int(fields, 'mode')} "
                  f"state={axis_int(fields, 'state')} "
                  f"vel={axis_int(fields, 'vel')} "
                  f"pos={axis_int(fields, 'pos')} "
                  f"status=0x{state_word:04x}"
                  f"{'  ← 给速度' if index == axis_index else ''}")
            if (state_word & STATUS_MASK) != STATUS_OPERATION_ENABLED:
                print(f"  注意：轴{index + 1} 未处于使能态（状态字 {state_word:#06x}），"
                      f"60FF 不会有实际输出")

        print(f"\n[zero] 零速段 {args.zero:g}s：目标是 0，轴应当不动")
        ok = hold("zero", args.zero, ZERO_FRAME, elapsed)
        elapsed += args.zero

        if ok:
            print(f"\n[drive] 轴{args.axis} 发 {args.velocity:+d} counts/s，保持 {args.hold:g}s")
            ok = hold("drive", args.hold, args.velocity, elapsed)
            elapsed += args.hold

        print(f"\n[zero-back] 回 0，确认 {args.zero_back:g}s")
        hold("zero-back", args.zero_back, ZERO_FRAME, elapsed)
    except (ConnectionError, OSError) as error:
        print(f"\n[异常] 连接断了：{error}", flush=True)
        print("主站可能已经自己退出了（目标被拒/会话中止/通信故障）。"
              "先看主站日志和报告，不要再连。", flush=True)
        report.summary(axis_index, args.velocity)
        return 1
    finally:
        # 尽力回零：任何正常或异常的收尾都补一条全 0 目标。连不上就算了，
        # 200 ms 之后主站自己也会把目标回零。
        try:
            if link.sock is not None:
                values = frame(ZERO_FRAME)
                if values:
                    link.send_frame(values)
        except OSError:
            pass

    report.summary(axis_index, args.velocity)
    print("\n下一步：立刻停主站（正常停机才会写报告）——")
    print("  echo shutdown | nc -U " + args.socket)
    return 0


if __name__ == "__main__":
    sys.exit(main())
