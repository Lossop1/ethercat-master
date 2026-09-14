#!/usr/bin/env python3
"""按最终场景的入口与流向驱动轴：Unix 域套接字 -> set_external_target。

流向与最终部署一致：客户端 -> 命令套接字 -> 主站外部目标缓冲区 ->
周期回调 position_target_source -> CSP 目标。不启用部署里的内部运动曲线，
也不绕过命令协议直接改目标。格式（协议 v1 文本）是当前的临时形态，入口和
流向不变。

用法:
  python3 test_external_motion_client.py <socket_path> [选项]

选项:
  --degrees D    各轴相对位移（输出轴度数，默认 30）
  --duration S   位移耗时（秒，默认 3）
  --rate HZ      目标更新频率（默认 50，必须快于 200ms 超时）
  --hold S       到位后保持发送的秒数（默认 60）
  --ready S      等待 RUNNING 的超时（秒，默认 30）

退出时停止发送，主站自动进入 HOLD（保持最后目标），不会停机。
"""

import argparse
import socket
import sys
import time

IDLE_TIMEOUT_S = 5.0
TARGET_TIMEOUT_MS = 200


class Client:
    def __init__(self, socket_path, ready_timeout):
        self.socket_path = socket_path
        self.ready_timeout = ready_timeout
        self.sock = None
        self.stream = None

    def connect(self):
        self.close()
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(self.socket_path)
                self.stream = self.sock.makefile("rwb")
                return
            except OSError as error:
                self.close()
                if time.monotonic() > deadline:
                    raise SystemExit(f"连接失败: {error}")
                time.sleep(0.2)

    def close(self):
        if self.stream is not None:
            try:
                self.stream.close()
            except OSError:
                pass
            self.stream = None
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def command(self, text):
        """发送一条命令并读回响应。连接失效时抛 OSError。"""
        if self.stream is None:
            self.connect()
        try:
            self.stream.write((text + "\n").encode("utf-8"))
            self.stream.flush()
            line = self.stream.readline()
        except OSError:
            self.close()
            raise
        if not line:
            self.close()
            raise ConnectionError("服务端关闭了连接")
        return line.decode("utf-8", errors="replace").strip()

    def wait_running(self):
        deadline = time.monotonic() + self.ready_timeout
        while True:
            response = self.command("status")
            state = parse_field(response, "state")
            if state == 4:
                return response
            if time.monotonic() > deadline:
                raise SystemExit(f"等待 RUNNING 超时，最后状态: {response}")
            time.sleep(0.1)


def parse_field(response, key):
    """从 OK|k=v k=v 形式的响应里取整数值。"""
    if not response.startswith("OK|"):
        return None
    for token in response[3:].replace(",", " ").split():
        if token.startswith(key + "="):
            value = token[len(key) + 1:]
            try:
                return int(value, 0)
            except ValueError:
                return None
    return None


def parse_axis_positions(response):
    """取每轴实际位置，返回按总线位置排序的列表。"""
    positions = []
    for segment in response.split("|"):
        if not segment.startswith("a"):
            continue
        fields = dict(
            item.split("=", 1) for item in segment.split(":", 1)[1].split(",") if "=" in item
        )
        if "pos" in fields:
            positions.append(int(fields["pos"], 0))
    return positions


def counts_per_degree(topology):
    """按拓扑返回的编码器分辨率和减速比换算 counts/度（输出轴）。

    减速比字段是 "电机转数/输出轴转数"（如 gear=28/1），大角度按输出轴度量。
    """
    enc = parse_field(topology, "enc") or 16384
    gear = 28.0
    if topology.startswith("OK|"):
        for token in topology.replace(",", " ").split():
            if token.startswith("gear=") and "/" in token:
                numerator, _, denominator = token[len("gear="):].partition("/")
                try:
                    if float(denominator) != 0.0:
                        gear = float(numerator) / float(denominator)
                except ValueError:
                    pass
                break
    return enc * gear / 360.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket_path")
    parser.add_argument("--degrees", type=float, default=30.0)
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--rate", type=float, default=50.0)
    parser.add_argument("--hold", type=float, default=60.0)
    parser.add_argument("--ready", type=float, default=30.0)
    args = parser.parse_args()

    if args.rate <= 0 or 1.0 / args.rate >= TARGET_TIMEOUT_MS / 1000.0:
        raise SystemExit("更新频率必须快于外部目标超时（200ms）")

    client = Client(args.socket_path, args.ready)
    client.connect()

    status = client.wait_running()
    topology = client.command("topology")
    start = parse_axis_positions(status)
    if not start:
        raise SystemExit(f"无法从 status 解析轴位置: {status}")

    per_degree = counts_per_degree(topology)
    delta = int(round(args.degrees * per_degree))
    targets = [position + delta for position in start]
    print(f"[client] counts/度={per_degree:.2f} 位移={delta} counts", flush=True)
    print(f"[client] 起点={start}", flush=True)
    print(f"[client] 终点={targets}", flush=True)

    period = 1.0 / args.rate
    steps = max(1, int(args.duration * args.rate))
    # 单步限幅：主站在周期线程里按相邻两条目标之差判定，超限会判 MOTION_INVALID
    # 并中止整个会话（协议 7.2 节）。步长按每次更新的增量折算，超过 max_step 时
    # 拉长位移时间而不是拒绝。
    max_step = parse_field(topology, "max_step") or 0
    if max_step > 0 and abs(delta) > 0:
        per_update = (abs(delta) + steps - 1) // steps
        if per_update > max_step:
            steps = (abs(delta) + max_step - 1) // max_step
            period = max(period, args.duration / steps) if steps else period
            print(f"[client] 单步 {per_update} counts 超过 max_step={max_step}，"
                  f"改为 {steps} 步（每步约 {abs(delta) // steps} counts）", flush=True)
    sent = 0
    errors = 0
    try:
        for step in range(1, steps + 1):
            frame = [position + delta * step // steps for position in start]
            response = client.command("set_external_target " + " ".join(str(v) for v in frame))
            sent += 1
            if not response.startswith("OK|"):
                errors += 1
                print(f"[client] 第 {step} 步被拒: {response}", flush=True)
                if "Not ready" in response:
                    client.wait_running()
            if step % max(1, steps // 10) == 0:
                print(f"[client] 位移 {step}/{steps} 目标={frame}", flush=True)
            time.sleep(period)

        print(f"[client] 到位，保持 {args.hold}s（每 {period:.3f}s 发送一次，避免 200ms 超时转 HOLD）",
              flush=True)
        hold_until = time.monotonic() + args.hold
        feedback = client.command("status")
        if feedback.startswith("OK|"):
            print(f"[client] 保持中实际位置={parse_axis_positions(feedback)}", flush=True)
        next_report = time.monotonic() + 5.0
        while time.monotonic() < hold_until:
            client.command("set_external_target " + " ".join(str(v) for v in targets))
            sent += 1
            time.sleep(period)
            if time.monotonic() >= next_report:
                feedback = client.command("status")
                if feedback.startswith("OK|"):
                    print(f"[client] t+{args.hold - (hold_until - time.monotonic()):.0f}s "
                          f"实际位置={parse_axis_positions(feedback)}", flush=True)
                next_report += 5.0
    except (OSError, ConnectionError) as error:
        print(f"[client] 发送中断: {error}", flush=True)

    client.close()
    print(f"[client] 结束：发出 {sent} 条目标，被拒 {errors} 条", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
