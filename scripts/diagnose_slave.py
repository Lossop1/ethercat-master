#!/usr/bin/env python3
"""通过主站程序的详细日志诊断从站问题"""
import subprocess
import sys

# 通过 pi.py 在 Orange Pi 上执行
result = subprocess.run([
    "python", "scripts/pi.py", "run",
    "cd ~/ethercat-master && sudo build/tools/master/emaster-master --deployment orangepi-current-bench --verbose 2>&1 | head -100",
    "--pty"
], capture_output=True, text=True, timeout=30)

print(result.stdout)
if result.stderr:
    print("STDERR:", result.stderr, file=sys.stderr)
