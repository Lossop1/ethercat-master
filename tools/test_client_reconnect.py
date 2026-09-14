#!/usr/bin/env python3
"""
interactive_control 客户端重连逻辑的离线测试。

不连真主站、不碰电机：起一个模拟服务端，复刻 command_server.c 的关键行为
（单客户端、5 秒空闲后单方面关闭连接），然后验证客户端的两条重连路径：

  1. 主动路径：发送前发现闲置超时，先重连再发
  2. 反应路径：闲置检查没抓到（阈值调大），send 撞上 EPIPE 后重连重试

用法：
    python3 tools/test_client_reconnect.py
"""

import os
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import interactive_control as ic


class MockServer(threading.Thread):
    """复刻主站命令服务器的连接语义。"""

    def __init__(self, path, idle_timeout_s):
        super().__init__(daemon=True)
        self.path = path
        self.idle_timeout_s = idle_timeout_s
        self.accepted = 0
        self.running = True

        if os.path.exists(path):
            os.unlink(path)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(1)
        self.listener.settimeout(0.2)

    def run(self):
        while self.running:
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return

            self.accepted += 1
            conn.settimeout(self.idle_timeout_s)
            buffer = b""
            try:
                while self.running:
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        # 客户端闲置过久：单方面关闭，复刻真实服务端行为
                        break
                    if not chunk:
                        break
                    buffer += chunk
                    while b"\n" in buffer:
                        line, _, buffer = buffer.partition(b"\n")
                        cmd = line.decode().strip()
                        if cmd == "topology":
                            reply = b"OK|axes=2,max_step=6400|a1:bus=1,enc=16384,gear=28/1,torque=0|a2:bus=2,enc=16384,gear=28/1,torque=0\n"
                        elif cmd == "status":
                            reply = b"OK|state=4 cycle=1 axes=2 enabled=1 completed=0|a1:pos=0,vel=0,torque=0,status=0x1237,target_pos=0,planned=0,err=0x0000,state=5|a2:pos=0,vel=0,torque=0,status=0x1237,target_pos=0,planned=0,err=0x0000,state=5\n"
                        else:
                            reply = b"OK|echo\n"
                        conn.sendall(reply)
            except OSError:
                pass
            finally:
                conn.close()

    def stop(self):
        self.running = False
        try:
            self.listener.close()
        except OSError:
            pass
        if os.path.exists(self.path):
            os.unlink(self.path)


def check(label, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {label}" + (f"  ({detail})" if detail else ""))
    return condition


def main():
    failures = 0
    path = os.path.join(tempfile.gettempdir(), "emaster-mock-test.sock")

    print("=" * 72)
    print("客户端重连逻辑离线测试（模拟服务端，不接触硬件）")
    print("=" * 72)

    # 服务端 2 秒空闲关闭；客户端主动重连阈值调到 1 秒，确保主动路径先触发
    server = MockServer(path, idle_timeout_s=2.0)
    server.start()
    time.sleep(0.2)

    original_idle = ic.IDLE_RECONNECT_S
    try:
        client = ic.EtherCATClient(path)
        if not client.connect():
            print("无法连接模拟服务端，测试中止")
            return 1

        # --- 基线：正常收发 ---
        print("\n[1] 基线收发")
        response = client.command("ping")
        failures += not check("单条命令往返", response == "OK|echo", repr(response))

        # --- 主动重连路径 ---
        print("\n[2] 主动重连路径（闲置超过阈值，发送前先重连）")
        ic.IDLE_RECONNECT_S = 1.0
        accepted_before = server.accepted
        time.sleep(3.0)  # 超过服务端 2 秒空闲，连接已被服务端关掉
        response = client.command("ping")
        failures += not check("闲置后命令仍成功", response == "OK|echo", repr(response))
        failures += not check("确实重建了连接",
                              server.accepted > accepted_before,
                              f"accepted {accepted_before} -> {server.accepted}")

        # --- 反应重连路径 ---
        print("\n[3] 反应重连路径（闲置检查放过，send 撞 EPIPE 后重连）")
        ic.IDLE_RECONNECT_S = 999.0  # 关掉主动重连，逼出 EPIPE 分支
        accepted_before = server.accepted
        time.sleep(3.0)  # 服务端再次单方面关闭
        response = client.command("ping")
        failures += not check("EPIPE 后命令仍成功", response == "OK|echo", repr(response))
        failures += not check("确实重建了连接",
                              server.accepted > accepted_before,
                              f"accepted {accepted_before} -> {server.accepted}")

        # --- 拓扑解析（含 max_step）---
        print("\n[4] 拓扑解析")
        failures += not check("topology 成功", client.refresh_topology())
        failures += not check("max_step = 6400", client.max_step == 6400,
                              str(client.max_step))
        failures += not check("axis_count = 2", client.axis_count == 2,
                              str(client.axis_count))
        factor = client.counts_per_degree()
        failures += not check("counts/度 = 1274.31",
                              factor is not None and abs(factor - 1274.3111) < 0.01,
                              str(factor))

        # --- 步长与限幅的关系 ---
        print("\n[5] 斜坡步长")
        step = client.step_counts_for(20.0, 50.0)
        failures += not check("20°/s@50Hz 步长 ≈ 509.7", abs(step - 509.72) < 0.1,
                              f"{step:.2f}")
        step_fast = client.step_counts_for(400.0, 50.0)
        failures += not check("400°/s@50Hz 被限幅到 5120",
                              abs(step_fast - 5120.0) < 0.1, f"{step_fast:.2f}")
        failures += not check("步长不超过 max_step", step_fast <= client.max_step)

        # --- 解析多行/粘包 ---
        print("\n[6] 响应分帧（连续快速命令）")
        ok = all(client.command("ping") == "OK|echo" for _ in range(5))
        failures += not check("连续 5 条命令均正确分帧", ok)

        client.disconnect()

    finally:
        ic.IDLE_RECONNECT_S = original_idle
        server.stop()

    print("\n" + "=" * 72)
    if failures:
        print(f"失败 {failures} 项")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
