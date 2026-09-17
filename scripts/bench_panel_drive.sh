#!/bin/bash
# 台架：不用人守着键盘，把一串按键喂给面板跑一遍。
#
# script(1) 给面板造一个真 pty（面板要求 stdin 是终端），我们从管道里按节奏喂按键，
# 面板的原始输出（含转义序列）落到 /tmp/panel-out.txt。转录里能验的是：
#   1) 进了备用屏幕、每帧用绝对定位、面板自己的输出里没有 \r\n（⇒ 物理上不会滚屏）；
#   2) 打字窗口里面板的控制拍还在推进（帧里的拍号在涨）；
#   3) 退出后面板把终端原样还回去、主站仍在跑。
# 转录用 scripts/analysis/panel_transcript.py 解析。
#
# 一句话别指望这条脚本能验：主站日志没有时间戳，"某次 [MODE] 切换是不是打字造成的"
# 判不出来（见 layering-plan.md P10.4）。面板不发目标时主站必然超时回 HOLD ——
# 那是看门狗在干活，不是故障，数它有几对说明不了问题。
#
# 前置：主站已经在跑（本脚本不负责起主站）。用 --range 3 把软范围压到 ±3°，
# 于是往返验证的默认振幅是 min(30, 3×0.6)=1.8°，全程都是小动作。
cd /home/orangepi/ethercat-master || exit 1

RANGE=${RANGE:-3}
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
OUT=/tmp/panel-out.txt
LOG=/tmp/emaster-console-master.log
MARK=/tmp/panel-mark.txt

date +%s > "$MARK"          # 打字窗口的起点，事后用它切主站日志
: > "$OUT"

{
    sleep 8                                  # 等面板接上主站、画出第一帧
    printf 'g';      sleep 1                 # 打开输入行
    printf '1:0.5 4:0.5'; sleep 1            # 多轴写法：轴1、轴4 各走 0.5°
    printf '\r';     sleep 4                 # 回车提交
    printf 't';      sleep 20                # 往返验证（振幅 1.8°）
    printf 't';      sleep 3                 # 停往返
    printf '0';      sleep 3                 # 回启动位置
    printf 'Q';      sleep 2                 # 退出面板
} | script -q -c \
    "stty cols 80 rows 24; timeout 60 python3 tools/emaster_console.py --deployment $DEPLOYMENT --range $RANGE" \
    "$OUT"

echo "面板转录：$(wc -c < "$OUT") 字节，$(grep -c $'\x1b\[[0-9]*;1H' "$OUT" 2>/dev/null || echo 0) 行绝对定位"
