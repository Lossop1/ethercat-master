#!/usr/bin/env python3
"""
测试外部位置控制功能

用法：
    python3 test_external_control.py [socket_path]

默认socket路径：/tmp/emaster-orangepi-bench-dual.sock
"""

import socket
import sys
import time

def send_target(sock_path, positions):
    """发送目标位置到主站"""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(sock_path)

        # 构造命令
        command = "set_external_target " + " ".join(str(p) for p in positions) + "\n"
        sock.send(command.encode())

        # 接收响应
        response = sock.recv(1024).decode().strip()
        sock.close()

        return response
    except Exception as e:
        return f"Error: {e}"

def main():
    sock_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/emaster-orangepi-bench-dual.sock"

    print(f"Testing external position control via {sock_path}")
    print("=" * 60)

    # 测试1：发送初始位置
    print("\nTest 1: Initial position")
    pos1 = [10000, 450000]
    response = send_target(sock_path, pos1)
    print(f"  Sent: {pos1}")
    print(f"  Response: {response}")
    time.sleep(2)

    # 测试2：发送不同位置
    print("\nTest 2: Move to different position")
    pos2 = [20000, 460000]
    response = send_target(sock_path, pos2)
    print(f"  Sent: {pos2}")
    print(f"  Response: {response}")
    time.sleep(2)

    # 测试3：连续更新
    print("\nTest 3: Continuous updates")
    for i in range(5):
        positions = [25000 + i * 500, 465000 + i * 500]
        response = send_target(sock_path, positions)
        print(f"  #{i+1}: {positions} -> {response}")
        time.sleep(0.5)

    print("\n" + "=" * 60)
    print("Test completed!")

if __name__ == "__main__":
    main()
