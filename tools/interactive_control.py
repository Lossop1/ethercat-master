#!/usr/bin/env python3
"""
EtherCAT 主站交互式位置控制工具

用法：
    python3 interactive_control.py [socket_path]

功能：
    - 实时查看电机状态（位置、速度、力矩、状态字）
    - 交互式设置目标位置（支持角度或 counts）
    - 持续模式：以指定频率持续发送目标
    - 安全检查：等待主站就绪后才允许发送目标

默认 socket 路径：/tmp/emaster-orangepi-bench-dual.sock
"""

import socket
import sys
import time
import re
import threading
import select

class EtherCATClient:
    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.connected = False
        self.state = None
        self.axes = []
        self.topology = None

    def connect(self):
        """连接到主站"""
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(5.0)
            self.sock.connect(self.sock_path)
            self.connected = True
            return True
        except Exception as e:
            print(f"连接失败: {e}")
            return False

    def disconnect(self):
        """断开连接"""
        if self.sock:
            try:
                self.sock.close()
            except:
                pass
        self.connected = False

    def send_command(self, cmd):
        """发送命令并接收响应"""
        if not self.connected:
            return None

        try:
            self.sock.sendall((cmd + "\n").encode())
            resp = self.sock.recv(4096).decode().strip()
            return resp
        except Exception as e:
            print(f"通信错误: {e}")
            self.connected = False
            return None

    def wait_for_ready(self):
        """等待主站进入 RUNNING 状态"""
        print("等待主站就绪 (state=4)...")
        retry_count = 0
        while retry_count < 50:  # 最多等待 5 秒
            resp = self.send_command("status")
            if not resp:
                return False

            # 解析 state
            m = re.search(r'state=(\d+)', resp)
            if m:
                state = int(m.group(1))
                if state == 4:
                    print("主站已就绪 (RUNNING)")
                    return True
                else:
                    state_name = {0: "INITIALIZING", 1: "PRE_OPERATIONAL",
                                  2: "SAFE_OPERATIONAL", 3: "ENABLING",
                                  5: "STOPPING", 6: "FAULTED"}.get(state, "UNKNOWN")
                    print(f"  当前状态: {state} ({state_name}), 等待中...")

            time.sleep(0.1)
            retry_count += 1

        print("超时：主站未进入 RUNNING 状态")
        return False

    def get_status(self):
        """获取当前状态"""
        resp = self.send_command("status")
        if not resp or not resp.startswith("OK|"):
            return None

        payload = resp[3:]  # 去掉 "OK|"

        # 解析状态字段
        status = {}
        parts = payload.split('|')

        # 主站状态
        header = parts[0]
        for item in header.split():
            if '=' in item:
                k, v = item.split('=', 1)
                status[k] = v

        # 轴状态
        axes = []
        for part in parts[1:]:
            if part.startswith('a'):
                axis_data = {}
                items = part.split(':')
                axis_data['id'] = items[0]
                for item in items[1].split(','):
                    if '=' in item:
                        k, v = item.split('=', 1)
                        axis_data[k] = v
                axes.append(axis_data)

        status['axes'] = axes
        return status

    def get_topology(self):
        """获取拓扑信息"""
        resp = self.send_command("topology")
        if not resp or not resp.startswith("OK|"):
            return None

        payload = resp[3:]
        parts = payload.split('|')

        axes = []
        for part in parts[1:]:
            if part.startswith('a'):
                axis_data = {}
                items = part.split(':')
                axis_data['id'] = items[0]
                for item in items[1].split(','):
                    if '=' in item:
                        k, v = item.split('=', 1)
                        axis_data[k] = v
                axes.append(axis_data)

        return axes

    def set_target(self, positions):
        """设置目标位置"""
        cmd = "set_external_target " + " ".join(str(p) for p in positions)
        resp = self.send_command(cmd)
        return resp

    def counts_to_degrees(self, counts, axis_idx):
        """将 counts 转换为角度（输出轴）"""
        if not self.topology or axis_idx >= len(self.topology):
            return None

        axis = self.topology[axis_idx]
        enc = int(axis['enc'])
        gear_m = int(axis['gear'].split('/')[0])
        gear_s = int(axis['gear'].split('/')[1])

        counts_per_degree = (enc / 1.0) * (gear_m / gear_s) / 360.0
        return counts / counts_per_degree

    def degrees_to_counts(self, degrees, axis_idx):
        """将角度（输出轴）转换为 counts"""
        if not self.topology or axis_idx >= len(self.topology):
            return None

        axis = self.topology[axis_idx]
        enc = int(axis['enc'])
        gear_m = int(axis['gear'].split('/')[0])
        gear_s = int(axis['gear'].split('/')[1])

        counts_per_degree = (enc / 1.0) * (gear_m / gear_s) / 360.0
        return int(degrees * counts_per_degree)


def print_status(status, topology):
    """格式化打印状态"""
    print("\n" + "="*80)
    print(f"主站状态: state={status.get('state')} cycle={status.get('cycle')} "
          f"enabled={status.get('enabled')} completed={status.get('completed')}")
    print("-"*80)

    for axis in status['axes']:
        axis_id = axis['id']
        pos = int(axis['pos'])
        vel = int(axis['vel'])
        torque = int(axis['torque'])
        sw = axis['status']
        target = int(axis['target_pos'])
        err = axis['err']
        state = axis['state']

        # 转换为角度
        axis_idx = int(axis_id[1:]) - 1
        if topology and axis_idx < len(topology):
            topo = topology[axis_idx]
            enc = int(topo['enc'])
            gear_m = int(topo['gear'].split('/')[0])
            gear_s = int(topo['gear'].split('/')[1])
            counts_per_deg = (enc / 1.0) * (gear_m / gear_s) / 360.0
            pos_deg = pos / counts_per_deg
            target_deg = target / counts_per_deg

            print(f"{axis_id}: pos={pos:8d} ({pos_deg:7.2f}°) target={target:8d} ({target_deg:7.2f}°)")
            print(f"     vel={vel:5d} torque={torque:5d} status={sw} err={err} state={state}")
        else:
            print(f"{axis_id}: pos={pos:8d} target={target:8d}")
            print(f"     vel={vel:5d} torque={torque:5d} status={sw} err={err} state={state}")

    print("="*80)


def continuous_mode(client, positions, frequency):
    """持续模式：以指定频率持续发送目标"""
    print(f"\n持续模式：以 {frequency} Hz 频率发送目标")
    print(f"目标位置: {positions}")
    print("按 Ctrl+C 停止\n")

    interval = 1.0 / frequency
    count = 0
    start = time.time()

    try:
        while True:
            resp = client.set_target(positions)
            count += 1

            if count % 20 == 0:
                elapsed = time.time() - start
                actual_freq = count / elapsed
                print(f"已发送 {count} 次, 实际频率: {actual_freq:.1f} Hz")

            time.sleep(interval)

    except KeyboardInterrupt:
        elapsed = time.time() - start
        actual_freq = count / elapsed
        print(f"\n停止持续模式")
        print(f"总计: {count} 次, 耗时: {elapsed:.2f}s, 平均频率: {actual_freq:.1f} Hz")


def interactive_shell(client):
    """交互式命令行"""
    print("\n" + "="*80)
    print("交互式控制模式")
    print("="*80)
    print("\n可用命令:")
    print("  s, status          - 查看当前状态")
    print("  t, topology        - 查看拓扑信息")
    print("  g <p1> <p2> ...    - 设置目标位置 (counts)")
    print("  d <d1> <d2> ...    - 设置目标位置 (度)")
    print("  r <d1> <d2> ...    - 相对移动 (度)")
    print("  c <p1> <p2> ... <freq> - 持续模式 (counts, Hz)")
    print("  h, help            - 显示帮助")
    print("  q, quit            - 退出")
    print()

    current_positions = None

    while True:
        try:
            cmd = input("> ").strip()
            if not cmd:
                continue

            parts = cmd.split()
            action = parts[0].lower()

            if action in ['q', 'quit']:
                break

            elif action in ['h', 'help']:
                print("\n可用命令:")
                print("  s, status          - 查看当前状态")
                print("  t, topology        - 查看拓扑信息")
                print("  g <p1> <p2> ...    - 设置目标位置 (counts)")
                print("  d <d1> <d2> ...    - 设置目标位置 (度)")
                print("  r <d1> <d2> ...    - 相对移动 (度)")
                print("  c <p1> <p2> ... <freq> - 持续模式 (counts, Hz)")
                print("  h, help            - 显示帮助")
                print("  q, quit            - 退出")
                print()

            elif action in ['s', 'status']:
                status = client.get_status()
                if status:
                    print_status(status, client.topology)
                    # 记录当前位置
                    current_positions = [int(axis['pos']) for axis in status['axes']]
                else:
                    print("获取状态失败")

            elif action in ['t', 'topology']:
                topo = client.get_topology()
                if topo:
                    print("\n拓扑信息:")
                    for axis in topo:
                        print(f"  {axis['id']}: bus={axis['bus']} enc={axis['enc']} "
                              f"gear={axis['gear']} torque={axis['torque']}")
                    print()
                else:
                    print("获取拓扑失败")

            elif action == 'g':
                # 设置目标位置 (counts)
                if len(parts) < 2:
                    print("用法: g <p1> <p2> ...")
                    continue

                try:
                    positions = [int(p) for p in parts[1:]]
                    resp = client.set_target(positions)
                    print(f"响应: {resp}")
                except ValueError:
                    print("错误: 位置必须为整数")

            elif action == 'd':
                # 设置目标位置 (度)
                if len(parts) < 2:
                    print("用法: d <d1> <d2> ...")
                    continue

                try:
                    degrees = [float(d) for d in parts[1:]]
                    positions = [client.degrees_to_counts(d, i) for i, d in enumerate(degrees)]

                    if None in positions:
                        print("错误: 无法转换角度（拓扑信息缺失）")
                        continue

                    print(f"目标: {degrees} 度 -> {positions} counts")
                    resp = client.set_target(positions)
                    print(f"响应: {resp}")
                except ValueError:
                    print("错误: 角度必须为数字")

            elif action == 'r':
                # 相对移动 (度)
                if len(parts) < 2:
                    print("用法: r <d1> <d2> ...")
                    continue

                if not current_positions:
                    print("错误: 未知当前位置，请先运行 'status' 命令")
                    continue

                try:
                    deltas = [float(d) for d in parts[1:]]
                    delta_counts = [client.degrees_to_counts(d, i) for i, d in enumerate(deltas)]

                    if None in delta_counts:
                        print("错误: 无法转换角度（拓扑信息缺失）")
                        continue

                    positions = [current_positions[i] + delta_counts[i] for i in range(len(deltas))]

                    print(f"相对移动: {deltas} 度 -> 目标 {positions} counts")
                    resp = client.set_target(positions)
                    print(f"响应: {resp}")
                except ValueError:
                    print("错误: 角度必须为数字")

            elif action == 'c':
                # 持续模式
                if len(parts) < 3:
                    print("用法: c <p1> <p2> ... <freq>")
                    continue

                try:
                    positions = [int(p) for p in parts[1:-1]]
                    frequency = float(parts[-1])
                    continuous_mode(client, positions, frequency)
                except ValueError:
                    print("错误: 位置必须为整数，频率必须为数字")

            else:
                print(f"未知命令: {action}，输入 'help' 查看帮助")

        except KeyboardInterrupt:
            print("\n使用 'quit' 命令退出")
        except EOFError:
            break


def main():
    sock_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/emaster-orangepi-bench-dual.sock"

    print("EtherCAT 主站交互式位置控制工具")
    print("="*80)
    print(f"Socket 路径: {sock_path}")

    client = EtherCATClient(sock_path)

    # 连接
    if not client.connect():
        return 1

    # 等待就绪
    if not client.wait_for_ready():
        client.disconnect()
        return 1

    # 获取拓扑信息
    client.topology = client.get_topology()
    if client.topology:
        print("\n拓扑信息:")
        for axis in client.topology:
            print(f"  {axis['id']}: bus={axis['bus']} enc={axis['enc']} "
                  f"gear={axis['gear']}")

    # 显示初始状态
    status = client.get_status()
    if status:
        print_status(status, client.topology)

    # 进入交互式命令行
    try:
        interactive_shell(client)
    finally:
        client.disconnect()
        print("\n已断开连接")

    return 0


if __name__ == "__main__":
    sys.exit(main())
