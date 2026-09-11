#!/usr/bin/env python3
"""P2.1 验证：发一条外部目标命令，等 300ms 超时，确认切换到 HOLD"""
import socket
import time

SOCK = "/tmp/emaster-orangepi-current-bench-idle.sock"

def send(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.sendall(cmd.encode())
    resp = s.recv(512).decode().strip()
    s.close()
    return resp

# 发送一次外部目标
r = send("set_external_target 17996")
print("SET_EXTERNAL_TARGET response:", r)

# 等待 300ms，超过 200ms 超时阈值
print("Waiting 300ms for timeout...")
time.sleep(0.3)

# 再发一次确认 last_update 过期、available 已清零
r2 = send("status")
print("STATUS after timeout:", r2)

print("Done")
