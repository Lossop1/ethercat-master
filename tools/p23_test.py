#!/usr/bin/env python3
"""P2.3 验证：发单步超过限幅阈值（2560 counts）的外部目标，确认主站触发 MOTION_INVALID"""
import socket
import time
import os
import re

SOCK = "/tmp/emaster-orangepi-current-bench-idle.sock"

def send(cmd, timeout=3):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall(cmd.encode())
    resp = s.recv(512).decode().strip()
    s.close()
    return resp

print("等待主站 socket...")
for _ in range(40):
    if os.path.exists(SOCK):
        break
    time.sleep(0.5)
else:
    print("ERROR: socket 未出现，主站可能未启动")
    raise SystemExit(1)

time.sleep(0.2)

r = send("status")
print("当前状态:", r)

m = re.search(r'a1:pos=(-?\d+)', r)
if not m:
    print("ERROR: 无法解析 a1 位置")
    raise SystemExit(1)

current_pos = int(m.group(1))
print(f"当前位置: {current_pos} counts")

# 第一条：合法目标（单步 = 0，不超过阈值）
r1 = send(f"set_external_target {current_pos}")
print("第一条合法目标响应:", r1)
time.sleep(0.1)

# 确认已切到外部控制模式
r2 = send("status")
print("发合法目标后状态:", r2)

# 第二条：单步超过 2560 的目标（+5000 >> 2560）
big_target = current_pos + 5000
print(f"发大跳变目标: {big_target} counts (单步 5000 >> 阈值 2560)")
try:
    r3 = send(f"set_external_target {big_target}", timeout=2)
    print("大跳变目标响应:", r3)
except Exception as e:
    print(f"send 异常（主站可能已因 MOTION_INVALID 退出）: {e}")

time.sleep(0.5)

if not os.path.exists(SOCK):
    print("RESULT: socket 已消失，主站已退出 — 符合单步限幅拒绝预期")
else:
    print("RESULT: socket 仍在，主站未退出")
    r4 = send("status")
    print("最终状态:", r4)
