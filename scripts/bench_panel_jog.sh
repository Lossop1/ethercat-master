#!/bin/bash
# 台架：用 pty 模拟"按住点动键"，看轴是不是立刻起步、按着连续走、松手就停。
#
# 终端按住一个键 = 一次性按下 + 每 30ms 左右一个重复事件。这里就是照这个节奏
# 往 pty 里灌 '+'，然后灌 '-' 走回来。RANGE=15 是为了让 10°/s 跑满 1 秒也够地方。
# 前置：主站已经在跑。轴会真动，跑之前确认台架周围是安全的。
cd /home/orangepi/ethercat-master || exit 1

RANGE=${RANGE:-15}
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
OUT=/tmp/panel-jog.txt

: > "$OUT"

{
    sleep 8                                    # 等面板接上、画出第一帧
    printf '1'                                 # 选轴1
    sleep 1
    for _ in $(seq 1 45); do printf '+'; sleep 0.033; done   # "按住 += 1.5 秒
    sleep 2                                    # 松手，看它停不停
    for _ in $(seq 1 45); do printf '-'; sleep 0.033; done   # "按住 -= 1.5 秒
    sleep 2
    printf '0'                                 # 回启动位置
    sleep 3
    printf 'Q'
    sleep 2
} | script -q -c \
    "stty cols 80 rows 24; timeout 60 python3 tools/emaster_console.py --deployment $DEPLOYMENT --range $RANGE" \
    "$OUT"

python3 - "$OUT" <<'PY'
import re
import sys

raw = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
drawn = raw.split("\x1b[?1049l")[0]
frames = [f for f in drawn.split("\x1b[1;1H") if f.strip()]


def relatives(frame):
    m = re.search(r"相对 +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)°", frame)
    return [float(x) for x in m.groups()] if m else None


series = [(int(m.group(1)), r[0]) for f in frames
          if (r := relatives(f)) and (m := re.search(r"第 (\d+) 拍", f))]
print(f"帧数 {len(frames)}，带拍号的帧 {len(series)}")
if not series:
    raise SystemExit("没解析到帧")

print("轴1 相对角度（每 5 帧取一个）：")
print("  拍号      角度      与上一采样差")
last = None
for cycle, value in series[::5]:
    delta = "" if last is None else f"{value - last:+.2f}°"
    print(f"  {cycle:8d}  {value:+7.2f}°   {delta}")
    last = value

values = [v for _, v in series]
forward = max(values)
print(f"\n正向最远 {forward:+.2f}°（按住 1.5 秒，速度 10°/s ⇒ 期望 10° 上下）")
print(f"最终回到 {values[-1]:+.2f}°")
print("点动中的帧：" + str(sum(1 for f in frames if "点动中" in f)) + " 个")
print("备用屏幕 进/出：" + str(drawn.count("\x1b[?1049h")) + "/" + str(raw.count("\x1b[?1049l")))
PY
