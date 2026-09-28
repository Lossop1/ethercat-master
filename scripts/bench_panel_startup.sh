#!/bin/bash
# 台架：把面板扔进"主站还没进 RUNNING"那段窗口里空转，看它崩不崩。
# 这是 P12.16（面板一帧就死、主站没人看着跑到两根轴进故障）的实机回归。
#
# 为什么非得上台架：崩在**每帧都画**的页脚上——`_headroom_text` 拿选中轴的软范围余量，
# 余量要 counts/度的换算系数。而系数是 `refresh_topology()` 读回来的，`attach()` 在
# `master_state != 4` 那一句就提前 return 了，系数还是空的；同一时刻 `poll_status()`
# 已经把 5 条轴塞进了 `engine.axes`，于是 `_headroom_text` 的 `if not engine.axes`
# 拦不住，裸下标 `self.factor[0]` 抛 IndexError。这个窗口只有真主站才有。
#
# 本脚本自己把主站拉起来（走的就是用户踩到的那条路：面板 `--start` + `--obs`），
# 再自己把它 SIGINT 停掉，不留主站在跑。
#
# 为什么"转录里出现过「主站还在启动」"就够证明走进了窗口：那句话写在 attach() 里
# `poll_status()` 成功之后的下一句判断上。看到它 ⇒ poll_status 成功过 ⇒ 那一刻
# `engine.axes` 已经被 5 条轴填上了。而 `status` 的轴行是按 `plan->axis_count` 发的
# （session_control.c:707-715），跟 state 无关，会话答得上话就有——所以窗口里轴必非空。
# 2026-09-18 在台架上直接问过主站，回包是 `state=3 axes=5 轴行=5`。
#
# 能验的 / 不能验的，说清楚：
#   能验——真窗口里面板一路重画、不抛异常、轴表照画。空转期间不给任何按键，
#        所以每帧都是"刚接上/还没接上主站"这一段，不是被按键带过去的。
#   不能验——"修前那份代码在这里必然崩"。那由离线对照负责，跑：
#        python3 scripts/checks/console_engine_check.py --mutate-startup-window
#        两者合起来才是闭环：离线证明检查有判别力，这里证明真窗口进得去。
#   不能验——窗口有多长。它随主站初始化耗时变，本脚本只是空转够久去覆盖它；
#        判据二会告诉你这一轮到底覆没覆盖到。
#
# 全程**不发任何目标**：只有主站使能并保持位置，轴不动。
# 必须在 root 下运行（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）：
#   sudo bash /home/orangepi/ethercat-master/scripts/bench_panel_startup.sh
#
# 参数（环境变量）：
#   DEPLOYMENT  部署 ID，默认 orangepi-bench-quint-30deg（台架现有 5 个从站）
#   WAIT        空转秒数，默认 75。想多覆盖一会儿就调大。

set -u

REPO=/home/orangepi/ethercat-master
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
WAIT=${WAIT:-75}

OUT=/tmp/panel_startup.txt
MASTER_LOG=/tmp/emaster-console-master.log

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 root（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）" >&2
    exit 1
fi

cd "$REPO" || exit 1

# 清理上次遗留：先停主站，再删套接字，避免新主站绑到旧 inode 上
pkill -9 -f emaster-master 2>/dev/null
sleep 1
rm -f /tmp/emaster-*.sock
rm -f "$OUT" "$MASTER_LOG"

echo "=== 面板启动窗口 · 空转 ${WAIT}s · 不发任何目标 ==="
echo "转录=$OUT 主站日志=$MASTER_LOG"

# 不给按键，让它自己重画；到点了给一个 Q 退出。
# script(1) 是为了给面板一个真 tty：没有 tty 它拿不到终端尺寸，走的不是同一条路。
{ sleep "$WAIT"; printf 'Q'; sleep 2; } | script -q -c \
    "stty cols 100 rows 30; ulimit -r unlimited; timeout $((WAIT + 40)) bash scripts/console.sh --start --obs" \
    "$OUT"
echo "面板退出码=$?"

# 面板走了，主站还开着（没给 --allow-stop-master，面板不会关它）。按台架纪律立刻停，
# 走 SIGINT 让报告写出来。
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
pgrep -f emaster-master >/dev/null && echo "** 还有主站在跑" || echo "（已确认没有主站在跑）"

echo ""
echo "=== 判定 ==="

if [ ! -f "$OUT" ]; then
    echo "[失败] 没有转录文件——面板压根没起来"
    exit 1
fi

# 判据一：面板有没有抛异常。索引越界那条崩溃会留下 Traceback/IndexError。
if grep -qE "Traceback|IndexError" "$OUT"; then
    echo "[失败] 转录里有异常："
    grep -nE "Traceback|IndexError" "$OUT" | head -10
    exit 1
fi
echo "[通过] 转录里没有 Traceback / IndexError"

# 判据二：面板确实处在过那个窗口——它打过"主站还在启动（state=…）"这句话。
# 这句话只有 attach() 走到 master_state!=4 那一句才写得出来，而那是被行是排在
# poll_status() 成功之后的：看到它，就等于证明 engine.axes 当时非空。
# 少了这一条，"面板没崩"可能只是因为从头到尾没进窗口。
if grep -q "主站还在启动" "$OUT"; then
    echo "[通过] 转录里出现过启动窗口（主站还在启动 state≠4）"
else
    echo "[警告] 转录里没看到启动窗口——主站进 RUNNING 太快，这一轮没覆盖到，"
    echo "       把 WAIT 调大重跑，或确认主站是不是已经在跑（那样面板会直接接上）"
fi

# 判据三：面板画出了轴表，不是空跑一场。
if grep -q "轴1" "$OUT"; then
    echo "[通过] 面板画出了轴表"
else
    echo "[失败] 转录里没有轴表——面板起来了但没画出来"
    tail -30 "$OUT"
    exit 1
fi

# 判据四（信息）：主站那次拉起来成没成。注意面板崩了主站不会跟着退出，
# 所以"主站起来了"本身不能当成面板没崩。
echo ""
if [ -f "$MASTER_LOG" ]; then
    echo "主站日志尾部（供对照）："
    grep -E "\[SHUTDOWN\]|总线状态" "$MASTER_LOG" | tail -4
else
    echo "（没有主站日志 $MASTER_LOG）"
fi

echo ""
echo "结论：面板在启动窗口里活下来了"
