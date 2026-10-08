#!/bin/bash
# 空转一轮：主站进 OP、什么都不发、干等 IDLE 秒，再从命令口 shutdown。
#
# 用途是"把主站单独放在总线上看它稳不稳"：不发任何目标，没有客户端参与，
# 所以这一轮里出现的任何异常都只能是主站/驱动器/布线这一侧的。
# 起始目的是复现 P12.17（空转 22.4 秒后若干轴进 CiA402 Fault），后来成了
# 台架每次改线、扩轴、换驱动器之后的第一道关（P12.36/P12.39/P12.43/P12.47/P12.48
# 的读数都出自这份脚本跑出来的轮次）。
#
# 必须在 root 下运行（mlockall + SCHED_FIFO）。ulimit -r 由脚本自己提，调用式不带引号：
#   sudo IDLE=45 TAG=idle1 DEPLOYMENT=orangepi-bench-non-30deg \
#        bash /home/orangepi/ethercat-master/scripts/bench_idle.sh
# 台架当前是九轴，所以 DEPLOYMENT 一般显式给 orangepi-bench-non-30deg；不给则用下面的默认值。
#
# 慢轮次要留意两件事（都踩过）：
#   - 主站没起来时套接字不会出现，脚本会把主站日志尾部打出来再退出——这正是
#     P12.43/P12.48 那两轮"开局就没起来"的形状，别把它当成脚本坏了。
#   - 主站可能在空转窗口里自己掉了（掉出 OP 或崩溃），脚本会印一句"注意：空转期间
#     主站已经不在了"，那也是要观察的情形，不是脚本失败。
#
# 参数（环境变量）：
#   DEPLOYMENT  部署 ID
#   TAG         产物文件名后缀
#   IDLE        空转秒数
#
# 产物：/tmp/<TAG>_outcome.txt（全程输出）、/tmp/<TAG>_done（退出码）、
#       /tmp/<TAG>_master.log（主站日志）。退出码 0 = 一切正常。
# 注意报告是部署配置里 run_report_path 指的那一份（每轮覆盖），要留证据得先 cp 一份；
# 而且它是 root 属主的，非 root 读不了。

set -u

REPO=${REPO:-/home/orangepi/ethercat-master}
# 非交互的 bash 不读 .bashrc/.profile，ulimit -r 默认是 0，主站拿不到 SCHED_FIFO——
# 而部署声明了 realtime.required=true，它会**拒绝进入 OP**，整轮白跑，且失败信息要到
# 报告里才看得出来。sudo 也救不了，因为这是 rlimit 不是权限（硬上限本来就是 unlimited，
# 自己提就行）。调用方按头部注释写成 bash -c 'ulimit -r unlimited; ...' 时这段是空操作；
# 只写 sudo bash <脚本> 时由它自己补上，省得"忘了加前缀"变成一次白跑。
ulimit -r unlimited 2>/dev/null
if [ "$(ulimit -r)" != "unlimited" ]; then
    echo "错误：ulimit -r 仍是 $(ulimit -r)，主站会因为拿不到 SCHED_FIFO 拒绝进入 OP" >&2
    exit 64
fi

DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
TAG=${TAG:-idle1}
IDLE=${IDLE:-45}
# OBSERVATION 是否让主站开观测通道（1=开，默认 1），与 bench_reciprocate_30deg.sh 同一个
# 开关、同一个理由：慢速遥测（温度／母线电压／电流、驱动器自报的 0x607A）只在观测通道
# 开着时才轮询，关着的话报告里那几栏恒为「—」，事后无法回答「那一刻电压是多少」。
OBSERVATION=${OBSERVATION:-1}

SOCK=/tmp/emaster-${DEPLOYMENT}.sock
LOG=/tmp/${TAG}_master.log
OUT=/tmp/${TAG}_outcome.txt
DONE=/tmp/${TAG}_done

cd "$REPO" || exit 1
rm -f "$OUT" "$LOG" "$DONE"
exec >>"$OUT" 2>&1

finish() {
    echo "$1" > "$DONE"
    echo "退出码=$1"
    # 必须真的用这个码退出：调用方取的是 $?，只 echo 不 exit 的话进程返回 0，
    # 于是"空转跑满"与"主站中途掉了"在调用方眼里是同一个结局。理由与修法同
    # bench_reciprocate_30deg.sh 里的 finish()。
    exit "$1"
}

echo "=== 空转 $IDLE 秒（$TAG）==="
echo "开始时间 $(date '+%Y-%m-%d %H:%M:%S')"

# 注意：这里清的是**任何** emaster-master，不是只有本部署那一份。台架同一时刻只跑
# 一个部署，所以没改；真要同时跑两个部署，这一行得按 --deployment 收窄。
pkill -9 -f emaster-master 2>/dev/null
sleep 2
rm -f /tmp/emaster-*.sock

# 不写成 "$OBS_PREFIX ./build/..."：OBS_PREFIX 为空时会展开成一个空参数，主站会把
# 它当成可执行文件名（P12.11 在 gui_e2e 上踩过这个）。
if [ "$OBSERVATION" = "1" ]; then
    env EMASTER_OBSERVATION=1 ./build/tools/master/emaster-master --deployment "$DEPLOYMENT" > "$LOG" 2>&1 &
else
    ./build/tools/master/emaster-master --deployment "$DEPLOYMENT" > "$LOG" 2>&1 &
fi
MASTER_PID=$!
echo "主站 pid=$MASTER_PID"

for _ in $(seq 1 90); do
    [ -S "$SOCK" ] && break
    sleep 0.5
done
if [ ! -S "$SOCK" ]; then
    echo "错误：套接字未创建，主站启动失败"
    tail -30 "$LOG"
    pkill -9 -f emaster-master 2>/dev/null
    finish 1
    exit 1
fi

STATE=0
for _ in $(seq 1 90); do
    STATE=$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | grep -o 'state=[0-9]*' | head -1 | cut -d= -f2)
    [ "$STATE" = "4" ] && break
    sleep 1
done
if [ "$STATE" != "4" ]; then
    echo "错误：主站未进入 RUNNING，最后 state=$STATE"
    tail -30 "$LOG"
    pkill -9 -f emaster-master 2>/dev/null
    finish 1
    exit 1
fi
echo "主站已 RUNNING，当前 $(date '+%H:%M:%S')"
echo "起点：$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | tr '|' '\n' | grep '^a' | tr '\n' ' ')"

echo ""
echo "=== 空转 $IDLE 秒，全程不发任何命令 ==="
sleep "$IDLE"
echo "空转结束 $(date '+%H:%M:%S')"
echo "终点：$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | tr '|' '\n' | grep '^a' | tr '\n' ' ')"

STOP_RC=0
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "shutdown → 期望主站自行退出"
    echo "shutdown" | timeout 3 nc -U "$SOCK" > /dev/null 2>&1
    SELF=0
    for _ in $(seq 1 60); do
        if ! kill -0 "$MASTER_PID" 2>/dev/null; then SELF=1; break; fi
        sleep 0.5
    done
    if [ "$SELF" = "1" ]; then
        echo "命令口 shutdown 使主站自行退出"
    else
        echo "FAIL：shutdown 后 30 秒主站仍在运行"
        STOP_RC=1
    fi
else
    echo "注意：空转期间主站已经不在了（掉出 OP 或崩溃）——正是要观察的情形"
    STOP_RC=1
fi

if kill -0 "$MASTER_PID" 2>/dev/null; then
    pkill -9 -f emaster-master 2>/dev/null
    STOP_RC=1
fi

echo ""
echo "=== 主站日志里的异常 ==="
grep -n "故障\|Fault\|FAULT\|超时\|错误\|失败" "$LOG" | head -20

echo ""
echo "=== 完成 $TAG ==="
finish "$STOP_RC"
