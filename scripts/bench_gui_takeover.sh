#!/bin/bash
# 台架：界面接管命令通道、走一遍**小幅度**动作，把"界面上填的字真的到了轴上"验掉。
# 这是 P12.19（界面多轴输入框）与 P12.20（真机验证）的实机回归。
#
# 为什么需要这一轮：`--selftest-takeover` 这套动作计划此前**只在假主站上跑过**
# （scripts/checks/gui_offline_check.py）。假主站能证"框里的字被解成了目标、发到了
# 命令口"，证不到"真主站认这条命令、轴真的按它动了、别的轴没动"。后者只有台架能给。
#
# 动作计划（界面的自检计划，见 tools/emaster_gui/app.py 的 _schedule_selftest_drive）：
#   接管 → 点动 +0.2° → 点动 −0.2° → **多轴框里走 1:0.3** → 回启动位置 → 急停 → 复位 → 释放
# 全程只动 1 号轴，最大 0.3°（10°/s 下约 30 ms 的运动）；2–5 号轴一个目标都不发。
# 多轴那一拍走的是**输入框**那条路（往框里填字再按按钮），不是直接调动作。
#
# 判据（这一轮成没成）：
#   一 界面退出码 0，日志里没有"界面内部异常"（异常会被 excepthook 打成退出码 3）
#   二 报告里 1 号轴 `曾到过=+0.300°`——**这是观测帧里的实际位置**，不是界面自己的账。
#     多轴那一拍要没生效，最大偏移会停在点动给的 0.200°，这条当场红
#   三 2–5 号轴 `曾到过=+0.000°`——"没写到的轴不动"这条在真主站上也成立
#   四 关机前 1 号轴回到起点（±0.05° 以内），且已发目标数 > 0、单步没超限幅
#   五 主站被 SIGINT 有序停掉，五轴 AL 8/0、无掉出 OP，且**不留主站在跑**
#
# 能验的 / 不能验的：
#   能验——真主站上这条命令的语义：轴号、正负号、没写到的轴不动、同时起停。
#   不能验——曲线画出来的样子（P12.18 那一半只能靠眼睛，已在跨机界面上看过，见
#        docs/requirements/layering-plan.md 的 P12.18）。本脚本不碰界面上的图形。
#   不能验——幅度大时的行为。这里最大 0.3°，30° 那一档在 bench_gui_e2e.sh 的 drive 臂。
#
# **本脚本会让 1 号轴动 0.3°（一次）。** 涉及物理动作先确认。
# 必须在 root 下运行（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）：
#   sudo bash /home/orangepi/ethercat-master/scripts/bench_gui_takeover.sh
#
# 参数（环境变量）：
#   DEPLOYMENT  部署 ID，默认 orangepi-bench-quint-30deg（台架现有 5 个从站）
#   DUR         自检总时长（秒），默认 20。动作按比例摊在这段时间里，给接管留出余量；
#               调小了会出现"接管还没完成、目标就被投出去"，那是这一轮没跑好，不是代码错。

set -u

REPO=/home/orangepi/ethercat-master
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
DUR=${DUR:-20}

MASTER_LOG=/tmp/gui_takeover_master.log
GUI_LOG=/tmp/gui_takeover_gui.log

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 root（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）" >&2
    exit 1
fi

cd "$REPO" || exit 1

# 清理上次遗留：先停主站，再删套接字，避免新主站绑到旧 inode 上
pkill -9 -f emaster-master 2>/dev/null
sleep 1
rm -f /tmp/emaster-*.sock
rm -f "$MASTER_LOG" "$GUI_LOG"

echo "=== 界面接管 · 自检 ${DUR}s · 只动 1 号轴、最大 0.3° ==="
echo "主站日志=$MASTER_LOG 界面日志=$GUI_LOG"

ulimit -r unlimited
setsid bash -c "cd $REPO; exec env EMASTER_OBSERVATION=1 \
    $REPO/build/tools/master/emaster-master --deployment $DEPLOYMENT" \
    > "$MASTER_LOG" 2>&1 &
echo "主站已拉起（观测已开），等它进 RUNNING…"

S=""
for _ in $(seq 1 90); do
    S=$(timeout 3 nc -U "/tmp/emaster-$DEPLOYMENT.sock" <<< "status" 2>/dev/null \
        | grep -o 'state=[0-9]*' | head -1 | cut -d= -f2)
    [ "$S" = "4" ] && break
    sleep 1
done
if [ "$S" != "4" ]; then
    echo "错误：主站没进 RUNNING（最后 state=${S:-无}）——没动任何轴，直接收尾"
    tail -20 "$MASTER_LOG"
    pkill -9 -f emaster-master 2>/dev/null
    exit 1
fi
echo "主站 state=4（已进入 OP 并使能）"

OBS="/tmp/emaster-$DEPLOYMENT-obs.sock"
for _ in $(seq 1 40); do [ -S "$OBS" ] && break; sleep 0.25; done
if [ -S "$OBS" ]; then
    echo "观测套接字已在：$OBS"
else
    echo "** 观测套接字不在——观测开关没生效。"
    echo "   「曾到过」要靠观测帧里的实际位置，没有它这一轮判不了。先查开关再重跑。"
    pkill -9 -f emaster-master 2>/dev/null
    exit 1
fi

# 界面自己找命令口与观测口（给了 --deployment 就按部署 ID 推出来，都在本机 unix 上）。
# 无头：offscreen 后端，不需要显示器。
QT_QPA_PLATFORM=offscreen PYTHONIOENCODING=utf-8 PYTHONPATH="$REPO/tools" \
    timeout $((DUR + 60)) \
    python3 -m emaster_gui --deployment "$DEPLOYMENT" \
    --selftest "$DUR" --selftest-takeover --no-confirm \
    > "$GUI_LOG" 2>&1
RC=$?
echo "界面退出码=$RC（日志 $GUI_LOG）"

# 界面走了，主站还开着。按台架纪律立刻停，走 SIGINT 让报告写出来。
MPID=$(pgrep -f emaster-master | head -1)
if [ -n "$MPID" ]; then
    echo "主站 pid=$MPID，发 SIGINT"
    kill -INT "$MPID" 2>/dev/null
    for _ in $(seq 1 60); do
        kill -0 "$MPID" 2>/dev/null || break
        sleep 0.5
    done
fi
if pgrep -f emaster-master >/dev/null; then
    echo "** 主站没退出，强杀"
    pkill -9 -f emaster-master
else
    echo "主站已有序退出"
fi

echo ""
echo "=== 判定 ==="
FAILED=0

if [ "$RC" != "0" ]; then
    echo "[失败] 界面退出码 $RC（3 = 界面自己崩了）"
    tail -30 "$GUI_LOG"
    FAILED=1
fi

if grep -q "界面内部异常" "$GUI_LOG"; then
    echo "[失败] 日志里有「界面内部异常」："
    grep -n -A 12 "界面内部异常" "$GUI_LOG" | head -20
    FAILED=1
fi

REPORT=$(sed -n '/---- GUI 自检 ----/,/---- 自检结束 ----/p' "$GUI_LOG")
if [ -z "$REPORT" ]; then
    echo "[失败] 没拿到自检报告——界面没跑到收尾那一步"
    tail -30 "$GUI_LOG"
    exit 1
fi

echo "--- 界面自检报告 ---"
echo "$REPORT"
echo ""

AXIS1=$(echo "$REPORT" | grep "轴1:" | head -1)
if echo "$AXIS1" | grep -q "曾到过=+0.300°"; then
    echo "[通过] 1 号轴到过 +0.300°（多轴框里那一拍，是观测帧里的实际位置）"
else
    echo "[失败] 1 号轴的曾到过不是 +0.300°：$AXIS1"
    echo "       0.200 ⇒ 多轴框那一拍没生效；0.000 ⇒ 这一次连点动都没走到（先看接管时序）"
    FAILED=1
fi

for N in 2 3 4 5; do
    LINE=$(echo "$REPORT" | grep "轴$N:" | head -1)
    if echo "$LINE" | grep -q "曾到过=+0.000°"; then
        echo "[通过] $N 号轴一个目标都没发（曾到过 +0.000°）"
    else
        echo "[失败] $N 号轴动过了：$LINE"
        FAILED=1
    fi
done

STATE=$(echo "$REPORT" | grep "主站：state=" | head -1)
SENT=$(echo "$STATE" | grep -o "已发 [0-9]* 条目标" | grep -o "[0-9]*")
if [ -n "$SENT" ] && [ "$SENT" -gt 0 ]; then
    echo "[通过] 真的发了目标（$SENT 条）：$STATE"
else
    echo "[失败] 一条目标都没发：$STATE"
    FAILED=1
fi

# 收尾时 1 号轴应当回到起点（计划里有一拍"回启动位置"）。1 count ≈ 0.0008°，
# 304 counts ≈ 0.027°，留到 0.05° 是给"停稳"留的余量。
HOME_DEG=$(echo "$AXIS1" | grep -o "实际=[+-][0-9.]*°" | head -1 | grep -o "[+-][0-9.]*")
if [ -n "$HOME_DEG" ]; then
    OFF=$(echo "$HOME_DEG" | awk '{v=$1<0?-$1:$1; print (v<=0.05) ? "ok" : "bad"}')
    if [ "$OFF" = "ok" ]; then
        echo "[通过] 收尾时 1 号轴回到起点（实际=$HOME_DEG°，相对启动位置）"
    else
        echo "[失败] 收尾时 1 号轴不在起点：$HOME_DEG°"
        FAILED=1
    fi
else
    echo "[警告] 报告里没读到 1 号轴的实际位置，这一条没判"
fi

echo ""
echo "--- 主站侧 ---"
if [ -f "$MASTER_LOG" ]; then
    grep -E "\[SHUTDOWN\]|总线状态|AL state|EXCHANGE_FAILED" "$MASTER_LOG" | tail -8
    if grep -qiE "掉出|not in OP|AL state.*1A" "$MASTER_LOG"; then
        echo "** 主站日志里出现掉出 OP 的痕迹——这一轮不算干净，要单独看"
        FAILED=1
    else
        echo "[通过] 主站日志里没有掉出 OP 的痕迹"
    fi
else
    echo "[失败] 没有主站日志 $MASTER_LOG"
    FAILED=1
fi

pgrep -f emaster-master >/dev/null && { echo "** 还有主站在跑"; FAILED=1; } \
    || echo "（已确认没有主站在跑）"

echo ""
if [ "$FAILED" = "0" ]; then
    echo "结论：界面上填的字在真主站上成立——只动了 1 号轴，多轴那一拍到了 +0.300°"
    exit 0
fi
echo "结论：这一轮没全过，逐条看上面标 [失败] 的行"
exit 1
