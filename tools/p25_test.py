#!/usr/bin/env python3
"""P2.5 验证：Fault Reset（控制字 0x0080）

测试目标：验证完整的故障恢复路径 Fault → Fault Reset → Switch On Disabled

前置条件：
  - 驱动器必须处于 Fault 状态（CIA 402 状态 3）
  - 主站已启动且 socket 可用

测试流程：
  1. 等待主站启动
  2. 人工触发故障（例如：手动堵转电机、断开编码器、SDO 写非法值）
  3. 确认驱动器进入 Fault 状态
  4. 发送 fault_reset 命令
  5. 验证驱动器转入 Switch On Disabled（状态 1）

注意：
  - 故障触发需要人工操作，脚本无法自动化
  - 不同驱动器的故障恢复时序可能不同
"""
import socket
import time
import os
import re
import sys

SOCK = "/tmp/emaster-orangepi-current-bench-idle.sock"

CIA402_FAULT = 3
CIA402_SWITCH_ON_DISABLED = 1


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

# 读取初始状态
r = send("status")
print("当前状态:", r)
st = axis_state(r)
print(f"a1 当前状态: {st}")

if st == CIA402_FAULT:
    print(f"a1 已处于 FAULT 状态（{CIA402_FAULT}），可直接测试 Fault Reset")
else:
    print(f"\na1 当前不在 FAULT 状态。需要人工触发故障：")
    print("  1. 手动堵转电机（用手抓住输出轴）")
    print("  2. 断开编码器连接")
    print("  3. 通过 SDO 写入非法参数")
    print("  4. 触发跟随误差保护（快速移动驱动器）")
    print("\n触发故障后，重新运行此脚本")
    sys.exit(0)

# 发送 fault_reset 命令
print("\n=== 发送 Fault Reset ===")
r = send("fault_reset")
print("fault_reset 响应:", r)
if "ERROR" in r:
    print("ERROR: fault_reset 命令被拒绝")
    sys.exit(1)

# 轮询状态转换，最多等 3 秒
deadline = time.monotonic() + 3.0
final_st = None
while time.monotonic() < deadline:
    try:
        r = send("status")
        final_st = axis_state(r)
        if final_st != CIA402_FAULT:
            break
    except Exception:
        break
    time.sleep(0.1)

print(f"\nFault Reset 后 a1 状态: {final_st}")

if final_st == CIA402_SWITCH_ON_DISABLED:
    print(f"RESULT: Fault Reset 成功")
    print(f"  故障前: FAULT({CIA402_FAULT})")
    print(f"  故障后: SWITCH_ON_DISABLED({CIA402_SWITCH_ON_DISABLED})")
    print("\nP2.5 测试通过")
elif final_st == CIA402_FAULT:
    print(f"WARNING: 驱动器仍处于 FAULT 状态")
    print("  可能原因：")
    print("    - 故障条件未清除（例如：编码器仍断开，电机仍堵转）")
    print("    - 驱动器需要多次 Fault Reset")
    print("    - 驱动器固件不支持 Fault Reset")
    sys.exit(1)
else:
    print(f"ERROR: 期望 SWITCH_ON_DISABLED({CIA402_SWITCH_ON_DISABLED})，实际 {final_st}")
    sys.exit(1)
