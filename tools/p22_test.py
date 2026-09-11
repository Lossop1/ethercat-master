#!/usr/bin/env python3
"""P2.2 验证：发送超出跟随误差阈值（2560 counts）的外部目标，确认主站触发 FOLLOWING_ERROR"""
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

# 等 OP 稳定
time.sleep(3)

r = send("status")
print("当前状态:", r)

m = re.search(r'a1:pos=(-?\d+)', r)
if not m:
    print("ERROR: 无法解析 a1 位置")
    raise SystemExit(1)

current_pos = int(m.group(1))
print(f"当前位置: {current_pos} counts")

# 发送超出阈值 2560 counts 的目标（+50000 使驱动器无法在一个周期内跟上）
target = current_pos + 50000
print(f"发送目标: {target} counts (偏差 50000 >> 阈值 2560)")
try:
    r2 = send(f"set_external_target {target}", timeout=2)
    print("set_external_target 响应:", r2)
except Exception as e:
    print(f"send 异常（主站可能已因 FOLLOWING_ERROR 退出）: {e}")

time.sleep(0.5)

# 确认 socket 是否消失（主站已退出）
if not os.path.exists(SOCK):
    print("RESULT: socket 已消失，主站已退出 — 符合 FOLLOWING_ERROR 预期")
else:
    print("RESULT: socket 仍在，主站未退出")
