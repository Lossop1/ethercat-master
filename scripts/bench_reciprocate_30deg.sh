#!/bin/bash
# 台架连续往返：主站 + 外部目标客户端，命令流全程保持在 200ms 失活阈值以内。
#
# 流向是最终场景那一条：客户端 -> 命令套接字 -> set_external_target ->
# 外部目标缓冲区 -> position_target_source -> CSP 目标。不启用部署里的内部
# 运动曲线，也不直接改过程映像。
#
# 必须在 root 下运行（主站需要 mlockall + SCHED_FIFO，套接字由 root 创建）：
#   sudo bash -c 'nohup bash /home/orangepi/ethercat-master/scripts/bench_reciprocate_30deg.sh &'
#
# 运行时长可达数分钟，被 nohup 拉起时 ssh 会话断开不影响；全程输出追加到
# /tmp/<TAG>_outcome.txt，结束时写 /tmp/<TAG>_done 记录退出码，用 tail 观察。
#
# 参数（环境变量）：
#   DEPLOYMENT  部署 ID，默认 orangepi-bench-quint-30deg（台架现有 5 个从站）
#   DURATION    往返总时长（秒），默认 600
#   TRAVERSE    单程耗时（秒），默认 3
#   DEGREES     单轴位移（输出轴度数），默认 30
#   RATE        目标更新频率（Hz），默认 100
#   WAVE        波形 triangle|sine，默认 triangle
#   TAG         产物文件名后缀，默认 recip30
#
# 报告路径取自部署配置的 run_report_path，无需另行指定。

set -u

REPO=/home/orangepi/ethercat-master
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
DURATION=${DURATION:-600}
TRAVERSE=${TRAVERSE:-3}
DEGREES=${DEGREES:-30}
RATE=${RATE:-100}
WAVE=${WAVE:-triangle}
TAG=${TAG:-recip30}

SOCK=/tmp/emaster-${DEPLOYMENT}.sock
LOG=/tmp/${TAG}_master.log
OUT=/tmp/${TAG}_outcome.txt
DONE=/tmp/${TAG}_done
CLIENT_LOG=/tmp/${TAG}_client.log
STAMP=$(date '+%Y%m%d-%H%M%S')

# 报告路径从部署配置里读，避免换部署时忘了同步（四轴用的就是另一个路径）。
# 按 deployment_id 找文件，不能按文件名猜：文件名用下划线，ID 用连字符。
REPORT_REL=$(DEPLOYMENT="$DEPLOYMENT" python3 -c "
import json, os, glob, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob('${REPO}/config/deployments/*.json')):
    with open(path) as handle:
        if json.load(handle).get('deployment_id') == target:
            print(json.load(open(path))['run_report_path'])
            sys.exit(0)
sys.exit(1)
" 2>/dev/null)
if [ -z "$REPORT_REL" ]; then
    echo "错误：读不出部署 $DEPLOYMENT 的 run_report_path" >&2
    exit 1
fi
REPORT=${REPO}/${REPORT_REL}

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 root（主站要求 mlockall/SCHED_FIFO，套接字由 root 创建）" >&2
    exit 1
fi

cd "$REPO" || exit 1
rm -f "$OUT" "$LOG" "$DONE" "$CLIENT_LOG"
exec >>"$OUT" 2>&1

finish() {
    echo "$1" > "$DONE"
    echo "退出码=$1"
}

echo "=== 台架连续往返 $STAMP ==="
echo "部署=$DEPLOYMENT 时长=${DURATION}s 单程=${TRAVERSE}s 位移=${DEGREES}度 频率=${RATE}Hz 波形=$WAVE"
echo "主站日志=$LOG 客户端日志=$CLIENT_LOG"

# 清理上次遗留：先停主站，再删套接字，避免新主站绑到旧 inode 上
pkill -9 -f emaster-master 2>/dev/null
sleep 2
rm -f /tmp/emaster-*.sock

./build/tools/master/emaster-master --deployment "$DEPLOYMENT" > "$LOG" 2>&1 &
MASTER_PID=$!
echo "主站 pid=$MASTER_PID"

for _ in $(seq 1 60); do
    [ -S "$SOCK" ] && break
    sleep 0.5
done
if [ ! -S "$SOCK" ]; then
    echo "错误：套接字未创建，主站启动失败"
    tail -40 "$LOG"
    pkill -9 -f emaster-master 2>/dev/null
    finish 1
    exit 1
fi
echo "套接字已创建：$SOCK"

# 等 RUNNING(state=4)：进入 OP 并使能需要若干秒
STATE=0
for _ in $(seq 1 60); do
    STATE=$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | grep -o 'state=[0-9]*' | head -1 | cut -d= -f2)
    [ "$STATE" = "4" ] && break
    sleep 1
done
if [ "$STATE" != "4" ]; then
    echo "错误：主站未进入 RUNNING，最后 state=$STATE"
    tail -40 "$LOG"
    pkill -9 -f emaster-master 2>/dev/null
    finish 1
    exit 1
fi
echo "主站已 RUNNING"

echo "起点：$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | tr '|' '\n' | grep '^a' | tr '\n' ' ')"

echo ""
echo "=== 往返 $DURATION 秒 ==="
python3 -u "$REPO/scripts/test_external_motion_client.py" "$SOCK" \
    --cycles 0 --total "$DURATION" --traverse "$TRAVERSE" \
    --degrees "$DEGREES" --rate "$RATE" --wave "$WAVE" --ready 30 \
    > "$CLIENT_LOG" 2>&1
CLIENT_RC=$?
echo "客户端退出码=$CLIENT_RC（0=跑满时长且命令流连续，1=出现过 200ms 空档，2=主站中断了命令流）"
tail -6 "$CLIENT_LOG"

echo ""
echo "终点：$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | tr '|' '\n' | grep '^a' | tr '\n' ' ')"

# 停机走 SIGINT，不走套接字的 shutdown。
# 已知缺陷：套接字的 stop/shutdown 只置 report->stop_requested，这条路径不经过
# 安全门（session_supervisor 的 collect_safety_conditions），而 session_control.c
# 的退出分支要求 safety_denied——健康会话里 safety_denied 恒为 false，于是
# stop/shutdown 是空操作，会话不会结束，报告也写不出来。
# SIGINT 经 main.c 的 application_stop_requested 回调进入安全门，会真正停机。
# 这里仍然发一次 shutdown 并记录结果，用来观察该缺陷是否已被修复。
echo "shutdown" | timeout 3 nc -U "$SOCK" > /dev/null 2>&1
for _ in $(seq 1 10); do
    kill -0 "$MASTER_PID" 2>/dev/null || break
    sleep 0.5
done
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "shutdown 未使主站退出（已知缺陷），改发 SIGINT"
    kill -INT "$MASTER_PID" 2>/dev/null
    for _ in $(seq 1 60); do
        kill -0 "$MASTER_PID" 2>/dev/null || break
        sleep 0.5
    done
fi
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "SIGINT 后主站仍未退出，强制结束"
    pkill -9 -f emaster-master 2>/dev/null
else
    echo "主站已自行退出"
fi

ARCHIVE=${REPO}/runtime/reports/archive/${STAMP}-run-${TAG}.json
if [ -f "$REPORT" ]; then
    cp "$REPORT" "$ARCHIVE"
    echo "报告归档：$ARCHIVE ($(stat -c %s "$ARCHIVE") 字节)"
else
    echo "警告：报告未生成：$REPORT"
fi

echo ""
echo "=== 命令流是否全程有效（出现即说明中途转 HOLD）==="
if [ -f "$LOG" ] && grep -q "External target timeout" "$LOG"; then
    grep -n "External target timeout" "$LOG" | head -5
    echo "结论：命令流出现过空档，主站中途转为 HOLD"
else
    echo "结论：无 External target timeout，命令流全程有效"
fi
echo "模式切换："
grep -n "\[MODE\]" "$LOG" | head -10

echo ""
echo "=== 周期与 WKC ==="
grep -n "WKC\|首次不符\|总线状态" "$LOG" | tail -15

echo ""
echo "=== 完成 $TAG ==="
finish "$CLIENT_RC"
