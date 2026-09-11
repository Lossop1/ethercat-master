#!/usr/bin/env python3
"""P2.4 验证：Quick Stop（控制字 0x0002）和 Halt（控制字 bit8）

测试顺序：
  1. 确认驱动器处于 OPERATION_ENABLED（状态 5）
  2. 发 halt 1：驱动器应减速至零，状态仍为 OPERATION_ENABLED
  3. 发 halt 0：恢复跟随
  4. 发 quick_stop：驱动器应进入 QUICK_STOP_ACTIVE（状态 6）
"""
import socket
import time
import os
import re
import sys

SOCK = "/tmp/emaster-orangepi-current-bench-idle.sock"

CIA402_OPERATION_ENABLED  = 5
CIA402_QUICK_STOP_ACTIVE  = 6


def send(cmd, timeout=3):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall(cmd.encode())
    resp = s.recv(1024).decode().strip()
    s.close()
    return resp


def axis_state(status_str, axis=1):
    m = re.search(rf'\|a{axis}:[^|]*,state=(\d+)', status_str)
    return int(m.group(1)) if m else None


def axis_planned(status_str, axis=1):
    m = re.search(rf'\|a{axis}:[^|]*,planned=(-?\d+)', status_str)
    return int(m.group(1)) if m else None


def axis_vel(status_str, axis=1):
    m = re.search(rf'\|a{axis}:pos=-?\d+,vel=(-?\d+)', status_str)
    return int(m.group(1)) if m else None


# 等待 socket 出现
print("等待主站 socket...")
for _ in range(40):
    if os.path.exists(SOCK):
        break
    time.sleep(0.5)
else:
    print("ERROR: socket 未出现，主站可能未启动")
    sys.exit(1)

time.sleep(0.2)

# 立即读取计划目标（planned）并开始持续刷新外部目标缓冲区。
# 必须用 planned 而非 pos：demo 正弦波一旦启动就会使 planned 偏离 pos，
# 接管瞬间若用 pos 会产生 pos-planned 差值的跟随误差，导致主站立即退出。
# 每轮迭代先读状态，拿到最新 planned，发给外部目标缓冲区，再判断 enabled，
# 保证 break 时缓冲区里就是本轮 enabled=1 对应的 planned 值，接管误差为零。
r = send("status")
print("上电中状态:", r)
current_planned = axis_planned(r)
if current_planned is not None:
    print(f"当前计划目标: {current_planned} counts，开始预锁定外部目标")
else:
    print("初始 planned 尚不可读（驱动器未使能），等待使能后跟踪")

# 持续发送外部目标直到 enabled=1，防止超时（P2.1 保护间隔约 500ms）
print("等待驱动器使能（同时持续刷新外部目标）...")
deadline = time.monotonic() + 10.0
r = None
while time.monotonic() < deadline:
    try:
        r = send("status")
        current_planned = axis_planned(r)
        if current_planned is not None:
            r_stab = send(f"set_external_target {current_planned}")
            if "ERROR" in r_stab:
                print(f"WARNING: set_external_target 响应: {r_stab}")
        if "enabled=1" in r:
            break
    except Exception as e:
        print(f"  轮询异常: {e}")
    time.sleep(0.08)
else:
    print(f"ERROR: 10秒内未达到 enabled=1，最后状态: {r}")
    sys.exit(1)

print("初始状态:", r)
st = axis_state(r)
if st != CIA402_OPERATION_ENABLED:
    print(f"ERROR: a1 状态 {st}，期望 {CIA402_OPERATION_ENABLED}（OPERATION_ENABLED）")
    sys.exit(1)
print(f"a1 = OPERATION_ENABLED，外部目标已接管，可继续测试")

# ---- 测试 1：Halt ----
print("\n=== 测试 1：Halt ===")
r = send("halt 1")
print("halt 1 响应:", r)
if "ERROR" in r:
    print("ERROR: halt 1 命令被拒绝")
    sys.exit(1)

# 等驱动器减速（500 ms 足够 1ms 周期的驱动器减速）
time.sleep(0.5)
r = send("status")
st = axis_state(r)
vel = axis_vel(r)
print(f"halt 1 后：状态={st}, vel={vel}")

if st != CIA402_OPERATION_ENABLED:
    print(f"ERROR: halt 后状态应仍为 OPERATION_ENABLED({CIA402_OPERATION_ENABLED})，实际={st}")
    sys.exit(1)
if vel is not None and abs(vel) > 200:
    print(f"WARNING: vel={vel}，驱动器可能尚未完全停止（阈值 200 counts/cycle）")
else:
    print(f"vel={vel} ≈ 0，减速符合预期")

r = send("halt 0")
print("halt 0 响应:", r)
if "ERROR" in r:
    print("ERROR: halt 0 命令被拒绝")
    sys.exit(1)
time.sleep(0.3)
r = send("status")
vel2 = axis_vel(r)
print(f"halt 0 后：vel={vel2}")
print("Halt 测试通过")

# ---- 测试 2：Quick Stop ----
print("\n=== 测试 2：Quick Stop ===")
r = send("quick_stop")
print("quick_stop 响应:", r)
if "ERROR" in r:
    print("ERROR: quick_stop 命令被拒绝")
    sys.exit(1)

# 轮询，最多等 3 秒进入 QUICK_STOP_ACTIVE
deadline = time.monotonic() + 3.0
final_st = None
while time.monotonic() < deadline:
    try:
        r = send("status")
        final_st = axis_state(r)
        if final_st == CIA402_QUICK_STOP_ACTIVE:
            break
    except Exception:
        break
    time.sleep(0.1)

print(f"Quick Stop 后 a1 状态: {final_st}")
if final_st == CIA402_QUICK_STOP_ACTIVE:
    print(f"RESULT: Quick Stop 成功，a1 = QUICK_STOP_ACTIVE({CIA402_QUICK_STOP_ACTIVE})")
else:
    print(f"ERROR: 期望 QUICK_STOP_ACTIVE({CIA402_QUICK_STOP_ACTIVE})，实际 {final_st}")
    sys.exit(1)

print("\nP2.4 全部测试通过")
