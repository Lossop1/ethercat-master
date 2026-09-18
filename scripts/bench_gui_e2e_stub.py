#!/usr/bin/env python3
"""假主站：只为在**真机**上干跑一遍台架脚本 bench_gui_e2e.sh，不碰任何硬件。

为什么要有这个。台架脚本里"起主站"那一条命令，是它唯一能把观测开关递给主站的地方
——而上一轮恰恰是在这里漏了：脚本没设 EMASTER_OBSERVATION，于是观测环压根没分配、
观测套接字没建，两个界面臂的监控半边整场是空的，纸面上却算通过。

这件事已经用"仔细读过脚本"防过一次，没防住。所以改用会失败的东西来防：这个假主站
读那个环境变量，**只有它是 1 的时候才建观测套接字**——于是脚本里那句"套接字在不在"
的断言就真的有了判别力，而不是在给自己鼓掌。

干跑（香橙派上，root，**先确认没有真主站在跑**；证据与报告都指到 /tmp，别覆盖真台架的）：

    export MASTER_BIN=/home/orangepi/ethercat-master/scripts/bench_gui_e2e_stub.py
    export ARMS='obs' DUR=5 ROUNDS=1
    export RPT_DIR=/tmp/gui_e2e_dryrun_reports EVIDENCE_DIR=/tmp/gui_e2e_dryrun_evidence
    bash scripts/bench_gui_e2e.sh

这个文件在索引里带可执行位（100755），理由是全仓 Python 脚本都按 `python3 xxx.py` 调，
只有它要**顶替一个二进制**：台架脚本起主站前会查 `[ -x "$MASTER_BIN" ]`，不带这个位
就直接"找不到主站可执行文件"——干跑连第一行都跑不到。

要验"没建套接字时脚本会不会红"，加 `STUB_NO_OBS=1`（命令行 `--no-obs` 也行，
但**台架脚本的 MASTER_BIN 只收一个可执行文件路径**，塞不进参数，干跑用环境变量那份）。
要验停机路径会不会等，加 `STUB_HOLD=1`。

**它不假装是个真主站**：命令套接字只回一句够台架脚本认人的 status，别的命令一概不答。
所以干跑能证的是"开关递到了没有、套接字在不在、判定会不会红"，证不了任何控制行为。
"""

import os
import signal
import socket
import sys
import time

DEPLOY = os.environ.get("DEPLOY", "orangepi-bench-quint-30deg")
# 台架脚本给的是绝对路径，这里跟着它的规矩走
SOCK = f"/tmp/emaster-{DEPLOY}.sock"
OBS_SOCK = f"/tmp/emaster-{DEPLOY}-obs.sock"

# 两个开关也能从环境变量给。命令行那份是给人直接跑的；环境变量那份是给**台架脚本**
# 用的——它的 MASTER_BIN 只接受一个可执行文件路径（起主站前要查 [ -x ]），塞不进参数。
FORCE_NO_OBS = "--no-obs" in sys.argv[1:] or os.environ.get("STUB_NO_OBS") == "1"
HOLD = "--hold" in sys.argv[1:] or os.environ.get("STUB_HOLD") == "1"
obs_switch = os.environ.get("EMASTER_OBSERVATION", "")


def log(text):
    print(f"[stub] {text}", flush=True)


def listen(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    server.listen(4)
    server.settimeout(0.5)
    return server


stopping = False


def stop(_signum, _frame):
    global stopping
    log(f"收到 SIGINT（--hold={HOLD}）")
    if not HOLD:
        stopping = True


signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

cmd = listen(SOCK)
log(f"命令套接字已建 {SOCK}")

obs = None
# **这就是被验的那一条**：开关是 1 才建观测套接字，否则连文件都不出现——和真主站
# 一样（真主站关着的时候连缓冲都不分配）。
if obs_switch == "1" and not FORCE_NO_OBS:
    obs = listen(OBS_SOCK)
    log(f"观测套接字已建 {OBS_SOCK}")
else:
    log(f"观测套接字没建（开关={obs_switch!r} --no-obs={FORCE_NO_OBS}）")

log(f"环境里 EMASTER_OBSERVATION={obs_switch!r}")

while not stopping:
    try:
        connection, _ = cmd.accept()
    except socket.timeout:
        continue
    except OSError:
        break
    # 只说够台架脚本认人用的一句，别的命令一概不回——**不假装是个真主站**。
    try:
        connection.settimeout(1.0)
        connection.recv(4096)
        connection.sendall(b"OK|state=4 axis_count=5\n")
    except OSError:
        pass
    finally:
        connection.close()

for server in (cmd, obs):
    if server is None:
        continue
    try:
        server.close()
    except OSError:
        pass
for path in (SOCK, OBS_SOCK):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
log("退出")
time.sleep(0.1)
