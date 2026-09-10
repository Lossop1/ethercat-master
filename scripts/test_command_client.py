#!/usr/bin/env python3
"""简单的命令客户端，用于测试实时命令接收系统"""

import socket
import sys

def send_command(socket_path, command):
    """发送命令并接收响应"""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(socket_path)

        # 发送命令
        sock.sendall(command.encode('utf-8') + b'\n')

        # 接收响应
        response = sock.recv(1024).decode('utf-8').strip()
        sock.close()

        return response
    except Exception as e:
        return f"ERROR: {e}"

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: python3 test_command_client.py <socket_path> <command>")
        print("示例: python3 test_command_client.py /tmp/emaster-orangepi-current-bench.sock status")
        sys.exit(1)

    socket_path = sys.argv[1]
    command = sys.argv[2]

    response = send_command(socket_path, command)
    print(f"发送: {command}")
    print(f"响应: {response}")
