#!/usr/bin/env python3
"""CST（CiA402 模式 10）常值力矩冒烟：只走命令套接字。

流向与最终部署一致：本脚本 -> /tmp/emaster-<部署>.sock -> 外部目标缓冲区 ->
position_target_source -> 6071 target_torque。不绕过命令协议，也不碰主站内部状态。

流程（每一步都把现场打出来）：
  1. 从部署配置读 external_target_torque_limit_per_mille 当自己的上限（唯一真源）。
  2. 等 state=4，读一轮基线：每轴的 mode（6061）、torque（6077）、pos（6064）。
  3. 零力矩段：全程按 --rate 发全 0，同时监控。人在场的话这时推一下轴，
     确认是自由状态——"能安全地什么都不做"要先证明。
  4. 加力矩：只给 --axis 那一根发 --torque（额定力矩的千分比），其余轴照旧 0，
     保持 --hold 秒。
  5. 立刻回 0，再监控 --zero-back 秒确认回零。
  6. 打印采样汇总，并提示立刻把主站停掉（本脚本不代劳）。

安全（写在这里也写在操作步骤里）：
  - 力矩单位是额定力矩的千分比（6071 相对 6076），5% 额定 = 50。
  - --torque 超过部署里配的上限就直接拒绝运行，本脚本自己也不发越限的值。
    （越限在协议那一头是 MOTION_INVALID，会打掉整个会话，没有"只拒这一条"。）
  - 任何异常（目标被拒、连接断开、模式对不上）都立刻尽力补发一条全 0 目标。
  - 本脚本不停主站。收尾靠人工 SIGINT——正常的停机流程才会写报告。

用法:
  python3 scripts/cst_smoke.py <socket_path> --deployment <部署ID> [选项]

选项:
  --axis N       加力矩的轴号（总线序号，从 1 起；默认 1）
  --torque T     力矩目标，额定千分比（默认 50 = 5%）
  --hold S       加力矩保持时长（默认 2.0）
  --zero S       加力矩前的零力矩段时长（默认 3.0）
  --zero-back S  回零后的确认时长（默认 1.0）
  --rate HZ      目标下发频率（默认 50，必须快于 200ms 看门狗）
  --monitor S    读 status 的间隔（默认 0.5）
  --ready S      等待 RUNNING 的超时（默认 60）
  --dry-run      不连主站，只读配置、核对参数并打印计划
"""

import argparse
import os
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

from emaster_console import (  # noqa: E402
    STATUS_MASK,
    STATUS_OPERATION_ENABLED,
    deployment_selected_mode,
    find_config_document,
)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

# 力矩模式下驱动器自报的模式显示（6061）。不是 10 就说明模式没切过去，
# 这时发 6071 是被驱动器忽略的——看起来"没反应"，其实是根本没进去。
TORQUE_MODE_DISPLAY = 10

# RxPDO 里能随时扔掉的目标：全 0。位置/速度/力矩三种模式下都是"什么都不做"。
ZERO_FRAME = 0


def parse_status(response):
    """把 status 回应拆成 (头部, 各轴字段列表)。

    格式：OK|state=4 cycle=... axes=5 ...|a1:pos=...,vel=...,mode=10|a2:...
    字段是按名字取的，所以固件加字段不会打乱这里。
    """
    if not response or not response.startswith("OK|"):
        return None, []
    parts = response[3:].split("|")
    header = {}
    for item in parts[0].split():
        if "=" in item:
            key, value = item.split("=", 1)
            header[key] = value
    axes = []
    for part in parts[1:]:
        if not part.startswith("a"):
            continue
        _, _, fields = part.partition(":")
        values = {}
        for item in fields.split(","):
            if "=" in item:
                key, value = item.split("=", 1)
                values[key] = value
        axes.append(values)
    return header, axes


def axis_int(axis_fields, key):
    """取一个轴字段的整数值；没有就是 None（"没读到"和"读到 0"不是一回事）。"""
    value = axis_fields.get(key)
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


class Link:
    """命令套接字上的最小客户端：一条命令一行回应。"""

    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.buffer = b""

    def connect(self, timeout_s):
        """等到连上为止。重试间隔逐次加倍：主站只有一格 accept 队列，快节奏重连
        会把它一直占满，越试越连不上（`tools/emaster_console.py` 的 command 里
        记过同一条），隔一会儿再试反而有效。"""
        deadline = time.monotonic() + timeout_s
        delay = 0.5
        first_failure = None
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(5.0)
                self.sock.connect(self.sock_path)
                self.buffer = b""
                return
            except OSError as error:
                self.close()
                if first_failure is None:
                    first_failure = error
                if time.monotonic() > deadline:
                    raise SystemExit(
                        f"连接失败 {self.sock_path}: {first_failure}\n"
                        f"（重试了 {timeout_s:g}s。主站起来了吗？先看主站日志，"
                        f"不要反复重跑本脚本。）")
                time.sleep(delay)
                delay = min(delay * 2.0, 4.0)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def command(self, text):
        """发一条命令并读回一行。连接断了就把异常抛给调用方——这里不重连，
        因为"主站没了"正是要立刻停下来告诉人的事。"""
        if self.sock is None:
            raise ConnectionError("套接字未连接")
        self.sock.sendall((text + "\n").encode("utf-8"))
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("服务端关闭了连接")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line.decode("utf-8", "replace").strip()

    def send_frame(self, values):
        return self.command("set_external_target " + " ".join(str(v) for v in values))

    def status(self):
        return parse_status(self.command("status"))


class Report:
    """采样与打印。表打到 stdout 供现场看，汇总留给证据文件。"""

    def __init__(self):
        self.rows = []
        self.rejected = 0
        self.max_torque_read = 0
        self.min_position = None
        self.max_position = None

    def sample(self, phase, elapsed, sent, axes, axis_index):
        fields = axes[axis_index] if axis_index < len(axes) else {}
        torque = axis_int(fields, "torque")
        position = axis_int(fields, "pos")
        mode = axis_int(fields, "mode")
        state = axis_int(fields, "state")
        if torque is not None:
            self.max_torque_read = max(self.max_torque_read, abs(torque))
        if position is not None:
            self.min_position = position if self.min_position is None else min(
                self.min_position, position)
            self.max_position = position if self.max_position is None else max(
                self.max_position, position)
        self.rows.append((phase, elapsed, sent, torque, position, mode, state))
        return torque, position, mode, state

    def print_row(self, phase, elapsed, sent, torque, position, mode, state, extra=""):
        print(f"[{phase}] t={elapsed:5.2f}s 目标={sent:+5d} "
              f"读回torque={torque if torque is not None else '--':>5} "
              f"pos={position if position is not None else '--':>10} "
              f"mode={mode if mode is not None else '--'} "
              f"state={state if state is not None else '--'}{extra}", flush=True)

    def summary(self, axis_index, torque):
        print("\n—— 采样汇总 ——")
        phases = {}
        for phase, _, sent, read_torque, position, _, _ in self.rows:
            entry = phases.setdefault(phase, {"n": 0, "max": 0, "pos": []})
            entry["n"] += 1
            if read_torque is not None:
                entry["max"] = max(entry["max"], abs(read_torque))
            if position is not None:
                entry["pos"].append(position)
        for phase in ("zero", "drive", "zero-back"):
            entry = phases.get(phase)
            if not entry:
                continue
            moved = (max(entry["pos"]) - min(entry["pos"])) if entry["pos"] else 0
            print(f"  {phase:9s} 采样 {entry['n']:3d} 条，"
                  f"|力矩| 最大 {entry['max']:4d} 千分比，位置跨度 {moved} counts")
        print(f"  轴 {axis_index + 1}：位置范围 "
              f"{self.min_position}..{self.max_position} counts")
        print(f"  目标被拒 {self.rejected} 条")
        print(f"  （目标值都是 {torque} 千分比；回零段与零力矩段应当看不到它）")


def load_plan(args):
    """从部署配置里取模式和上限。返回 (mode, limit)。

    先判模式再判上限：模式不对的部署（位置模式那份就没有力矩上限）要报"模式不对"，
    不能报成"没有上限"——那会让人以为是配置缺了个字段。
    """
    deployment = find_config_document(
        str(REPO_ROOT), "deployments", "deployment_id", args.deployment)
    if deployment is None:
        raise SystemExit(f"找不到部署 {args.deployment}（config/deployments/）")
    mode = deployment_selected_mode(str(REPO_ROOT), args.deployment)
    if mode != "cst":
        raise SystemExit(
            f"这个部署选的是 {mode or '(读不出模式)'} 模式，不是 cst。"
            f"本脚本只发力矩，模式不对就别跑。")
    limit = deployment.get("external_target_torque_limit_per_mille")
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1000:
        raise SystemExit(
            f"部署 {args.deployment} 没有可用的 external_target_torque_limit_per_mille"
            f"（读到 {limit!r}）。力矩冒烟拒绝在没有上限的部署上跑。")
    return mode, limit


def selftest():
    """离线自检：只验 status 的解析，不连主站（本机没有 AF_UNIX 也能跑）。

    样本直接取 `docs/protocol/command-protocol-v1.md` 里那一行，另加一条带 mode=
    的——解析要是跟协议文档对不上，冒烟跑起来会把"没读到"和"读到 0"混成一样。
    """
    documented = ("OK|state=4 cycle=123456 axes=2 enabled=1 completed=0"
                  "|a1:pos=573362,vel=0,torque=0,status=0x1637,target_pos=573362,"
                  "planned=573362,err=0x0000,state=7"
                  "|a2:pos=389669,vel=0,torque=0,status=0x1637,target_pos=389669,"
                  "planned=389669,err=0x0000,state=7")
    cst = ("OK|state=4 cycle=7 axes=5 enabled=1 completed=0"
           "|a1:pos=-1234,vel=0,torque=-7,status=0x0027,target_pos=0,planned=0,"
           "err=0x0000,state=6,mode=10")
    failures = []

    def check(label, condition, detail=""):
        print(f"  [{'通过' if condition else '失败'}] {label} {detail}".rstrip())
        if not condition:
            failures.append(label)

    print("cst_smoke 离线自检（status 解析）")
    header, axes = parse_status(documented)
    check("协议文档里那一行解析得出头部",
          header is not None and header.get("state") == "4" and header.get("axes") == "2",
          str(header))
    check("两轴按顺序排开", len(axes) == 2, f"{len(axes)} 轴")
    check("十六进制状态字照 0x 读", axis_int(axes[0], "status") == 0x1637,
          hex(axis_int(axes[0], "status") or 0))
    check("位置带符号", axis_int(axes[1], "pos") == 389669)

    header, axes = parse_status(cst)
    check("新字段 mode 读得出来", axis_int(axes[0], "mode") == 10)
    check("力矩是负数照收", axis_int(axes[0], "torque") == -7)
    check("只有一根轴时也不串位", len(axes) == 1 and axis_int(axes[0], "pos") == -1234)
    check("字段缺了是 None，不是 0", axis_int(axes[0], "没有这个字段") is None)

    header, axes = parse_status("ERROR|NOT_RUNNING")
    check("错误回应不算状态", header is None and axes == [])

    if failures:
        print("\n自检失败项：" + "，".join(failures))
        return 1
    print("自检全部通过")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="CST 常值力矩冒烟（只走命令套接字）")
    parser.add_argument("socket", nargs="?", help="命令套接字路径")
    parser.add_argument("--deployment", help="部署 ID（读模式和上限）")
    parser.add_argument("--axis", type=int, default=1, help="加力矩的轴号（从 1 起）")
    parser.add_argument("--torque", type=int, default=50, help="力矩目标（额定千分比）")
    parser.add_argument("--hold", type=float, default=2.0, help="加力矩保持时长（秒）")
    parser.add_argument("--zero", type=float, default=3.0, help="零力矩段时长（秒）")
    parser.add_argument("--zero-back", dest="zero_back", type=float, default=1.0,
                        help="回零确认时长（秒）")
    parser.add_argument("--rate", type=float, default=50.0, help="目标下发频率（Hz）")
    parser.add_argument("--monitor", type=float, default=0.5, help="读 status 的间隔（秒）")
    parser.add_argument("--ready", type=float, default=60.0, help="等待 RUNNING 的超时（秒）")
    parser.add_argument("--dry-run", action="store_true", help="只核参数、不连主站")
    parser.add_argument("--selftest", action="store_true", help="离线自检 status 解析")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    if not args.deployment:
        raise SystemExit("缺少 --deployment（部署 ID 是模式和上限的唯一来源）")
    if args.rate <= 0.0 or args.monitor <= 0.0:
        raise SystemExit("--rate 和 --monitor 必须是正数")
    if abs(args.torque) > 1000:
        raise SystemExit("--torque 超出千分比量程（±1000）")

    mode, limit = load_plan(args)
    print(f"部署 {args.deployment}：模式 {mode}，力矩上限 ±{limit} 千分比")
    if abs(args.torque) > limit:
        raise SystemExit(
            f"--torque {args.torque} 超过部署上限 {limit}；越限在协议那头会打掉整个会话。")

    if args.dry_run:
        print(f"计划：零力矩段 {args.zero:g}s → 轴{args.axis} 发 {args.torque} 千分比 "
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
        """整帧：所有轴 0，只有选中的那根给 value。

        轴数是连上之后读 status 才知道的；还没读到就给空帧，让调用方自己别发。
        """
        if axis_count <= 0:
            return []
        values = [ZERO_FRAME] * axis_count
        if axis_index < axis_count:
            values[axis_index] = value
        return values

    def hold(phase, seconds, sent, elapsed_base, zero_back=False):
        """按 --rate 发同一帧，同时按 --monitor 读状态采样。返回是否全程被接受。"""
        deadline = time.monotonic() + seconds
        next_sample = time.monotonic()
        started = time.monotonic()
        ok = True
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
                    ok = False
                    break
                torque, position, mode_display, state = report.sample(
                    phase, elapsed_base + (now - started), sent, axes, axis_index)
                report.print_row(phase, elapsed_base + (now - started), sent,
                                 torque, position, mode_display, state)
                # 模式对不上就别再加力矩了：6071 会被静默忽略。
                if mode_display is not None and mode_display != TORQUE_MODE_DISPLAY:
                    print(f"[{phase}] 轴{args.axis} 的模式显示是 {mode_display}，"
                          f"不是 {TORQUE_MODE_DISPLAY}——停下", flush=True)
                    return False
            time.sleep(max(0.0, 1.0 / args.rate - (time.monotonic() - now)))
        return ok

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
            print(f"  轴{index + 1}: mode={axis_int(fields, 'mode')} "
                  f"state={axis_int(fields, 'state')} "
                  f"torque={axis_int(fields, 'torque')} "
                  f"pos={axis_int(fields, 'pos')} "
                  f"status=0x{axis_int(fields, 'status') or 0:04x}"
                  f"{'  ← 加力矩' if index == axis_index else ''}")
            state_word = axis_int(fields, "status") or 0
            if (state_word & STATUS_MASK) != STATUS_OPERATION_ENABLED:
                print(f"  注意：轴{index + 1} 未处于使能态（状态字 {state_word:#06x}），"
                      f"力矩不会有实际输出")

        print(f"\n[zero] 零力矩段 {args.zero:g}s：现在是自由状态，可以推一下轴")
        ok = hold("zero", args.zero, ZERO_FRAME, elapsed)
        elapsed += args.zero

        if ok:
            print(f"\n[drive] 轴{args.axis} 加 {args.torque} 千分比"
                  f"（额定力矩的 {args.torque / 10.0:g}%），保持 {args.hold:g}s")
            ok = hold("drive", args.hold, args.torque, elapsed)
            elapsed += args.hold

        print(f"\n[zero-back] 回 0，确认 {args.zero_back:g}s")
        hold("zero-back", args.zero_back, ZERO_FRAME, elapsed)
    except (ConnectionError, OSError) as error:
        print(f"\n[异常] 连接断了：{error}", flush=True)
        print("主站可能已经自己退出了（目标被拒/会话中止/通信故障）。"
              "先看主站日志和报告，不要再连。", flush=True)
        report.summary(axis_index, args.torque)
        return 1
    finally:
        # 尽力回零：任何正常或异常的收尾都补一条全 0 目标。连不上就算了，
        # 200 ms 之后主站自己也会把目标回零（C2）。
        try:
            if link.sock is not None:
                values = frame(ZERO_FRAME)
                if values:
                    link.send_frame(values)
        except OSError:
            pass

    report.summary(axis_index, args.torque)
    print("\n下一步：立刻停主站（正常停机才会写报告）——")
    print("  pkill -INT -x emaster-master  # 等约 15 秒，报告写完再退出")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
