#!/usr/bin/env python3
"""在 Orange Pi 上用伪终端采集硬件指纹，自动确认 PRE-OP。

expect 在 Windows 侧编写容易被行尾和编码破坏，这里用 paramiko 的 pty 通道直接
驱动远端程序：等提示出现后写入确认令牌，再把全部输出回显给调用者。
"""

from pathlib import Path
from __future__ import annotations

import argparse
import sys
import time

import paramiko

HOST = "192.168.137.54"
USER = "orangepi"
PASSWORD = "orangepi"

DEFAULT_BINARY = str(Path(__file__).resolve().parents[2] / "build/apps/fingerprint/emaster-fingerprint")
DEFAULT_OUTPUT = "/tmp/fingerprint-capture.json"
CONFIRM_TOKEN = "PRE-OP"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    # 部署 ID 不设默认值：交付的仓库里没有任何部署配置，写死一个 ID 只会在换
    # 现场之后静静地对不上。不给就交给指纹工具自己按主机名匹配。
    parser.add_argument("--deployment", default=None)
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASSWORD, timeout=10)

    command = f"sudo {args.binary} capture {args.output}"
    if args.deployment is not None:
        command += f" --deployment {args.deployment}"
    channel = client.get_transport().open_session()
    channel.get_pty()
    channel.exec_command(command)

    buffer = ""
    confirmed = False
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        if channel.recv_ready():
            chunk = channel.recv(4096).decode("utf-8", errors="replace")
            buffer += chunk
            sys.stdout.write(chunk)
            sys.stdout.flush()
        if not confirmed and "继续" in buffer:
            channel.sendall(CONFIRM_TOKEN + "\n")
            confirmed = True
        if channel.exit_status_ready() and not channel.recv_ready():
            break
        time.sleep(0.05)

    exit_code = channel.recv_exit_status()
    client.close()
    sys.stdout.write(f"\n[exit={exit_code}]\n")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
