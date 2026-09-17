#!/usr/bin/env python3
"""把面板 pty 转录拆开，逐条给出可判定的结论（配合 bench_panel_drive.sh / _jog.sh）。

用法：
    python scripts/analysis/panel_transcript.py <转录文件> [主站日志]

两个踩过的坑写在代码里，别再踩：
  * 判"轴4 有没有动"不能拿字符串比 "+0.00"，"-0.00" 会被算成动了；
  * 找拍号的正则不能写在 raw 串里（会变成字面反斜杠），永远匹配不上；
  * 绘制区要从**第一个** `\\x1b[?1049h`（进备用屏幕）算起。从文件头算起会把
    `script(1)` 自己写的头一并算进去，它的那个 `\\n` 会被误判成"面板有换行"。
"""
import re
import sys

if len(sys.argv) < 2:
    raise SystemExit(__doc__)

RAW = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
LOG = (open(sys.argv[2], encoding="utf-8", errors="replace").read().splitlines()
       if len(sys.argv) > 2 else None)

enter_at = RAW.find("\x1b[?1049h")         # 进备用屏幕 = 面板绘制区开始
exit_at = RAW.find("\x1b[?1049l")          # 退出备用屏幕 = 面板绘制区结束
drawn = RAW[enter_at:exit_at] if 0 <= enter_at < exit_at else RAW
tail = RAW[exit_at:] if exit_at >= 0 else ""

frames = [f for f in drawn.split("\x1b[1;1H") if f.strip()]
rows = {int(m) for m in re.findall(r"\x1b\[(\d+);1H", drawn)}
print(f"1) 绘制区 {len(drawn)} 字节 / {len(frames)} 帧；用到行号 {min(rows)}..{max(rows)}（终端 24 行）")
print(f"   绘制区里的回车/换行：\\r {drawn.count(chr(13))} 个，\\n {drawn.count(chr(10))} 个 "
      f"→ {'物理上不可能滚屏' if not (drawn.count(chr(13)) or drawn.count(chr(10))) else '有换行！'}")
print(f"   退出后的普通文字（该有换行）：\\r {tail.count(chr(13))} 个，\\n {tail.count(chr(10))} 个")
print(f"   备用屏幕 进 {RAW.count(chr(27) + '[?1049h')} 次 / 出 {RAW.count(chr(27) + '[?1049l')} 次；"
      f"光标 隐 {RAW.count(chr(27) + '[?25l')} / 现 {RAW.count(chr(27) + '[?25h')}")

def cycles_in(blob):
    return [int(m) for m in re.findall(r"第 (\d+) 拍", blob)]

typed = [f for f in frames if "回车走 / Esc 取消" in f]
nums = cycles_in("".join(typed))
print(f"\n2) 打字窗口：带输入行的帧 {len(typed)} 个，拍号 {nums[:3]} … {nums[-3:]}"
      f"（推进 {nums[-1] - nums[0] if len(nums) > 1 else 0} 拍）")
for frame in typed[:1] + typed[-1:]:
    line = re.search(r"> [^\x1b]*\x1b\[0m[^▏]*▏", frame) or re.search(r"> [^▏]*▏", frame)
    print("   输入行：" + (line.group(0).replace("\x1b[0m", "") if line else "?"))

def relatives(frame):
    m = re.search(r"相对 +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)°", frame)
    return [float(x) for x in m.groups()] if m else None

def targets(frame):
    m = re.search(r"目标 +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)° +([-+][\d.]+)°", frame)
    return [float(x) for x in m.groups()] if m else None

rels = [(f, relatives(f)) for f in frames]
both = [r for _, r in rels if r and abs(r[0]) > 0.02 and abs(r[3]) > 0.02]
print(f"\n3) 多轴：轴1、轴4 同时离开零点（|角度|>0.02°）的帧 {len(both)} 个")
if both:
    print(f"   第一帧 {both[0]}　最后一帧 {both[-1]}　轴4 最大 {max(abs(r[3]) for r in both):.2f}°")
axis4 = [r[3] for _, r in rels if r]
print(f"   轴4 全程范围 {min(axis4):+.2f}° .. {max(axis4):+.2f}°（命令 +0.5°）")

axis1 = [r[0] for _, r in rels if r]
print(f"\n4) 往返：轴1 实际位置范围 {min(axis1):+.2f}° .. {max(axis1):+.2f}°（振幅 1.8°）")
soak = re.findall(r"往返验证 · 轴(\d) 振幅 ([\d.]+)° · 已跑 ([\d:]+) · 往返 (\d+) 趟 · 异常 (\d+) 条", drawn)
print(f"   面板记账：{len(soak)} 帧，末条 {'/'.join(soak[-1]) if soak else '（没跑）'}")
print("   收尾：" + (re.search(r"往返验证结束：\d+ 趟，异常 \d+ 条", drawn) or
                   type("x", (), {"group": lambda s, i=0: "（没看到）"})()).group(0))
if LOG is None:
    raise SystemExit(0)                    # 没给日志就只出面板这一段

modes = [(i, line) for i, line in enumerate(LOG, 1) if "[MODE]" in line]
print(f"\n5) 主站日志 {len(LOG)} 行，[MODE] 事件 {len(modes)} 条：")
for i, line in modes:
    print(f"   {i:3} {line}")

# 这条日志没有时间戳，所以"某次切换是不是打字造成的"从它本身判不出来。
# 面板空闲时主站本来就该超时进 HOLD（200 ms 看门狗），所以 HOLD 的数量本身不是问题；
# 要判的是它有没有落在打字窗口里。没有时间戳就只能数对数、看有没有异常串。
switches = sum(1 for _, line in modes if "EXTERNAL control" in line)
timeouts = sum(1 for _, line in modes if "timeout" in line)
print(f"   进入 EXTERNAL {switches} 次 / 超时回 HOLD {timeouts} 次"
      f"（面板不发目标时主站必然超时，这是看门狗在干活）")
bad = [line for line in LOG if "released" in line.lower() or "abort" in line.lower()
       or "MOTION_INVALID" in line]
print("   断连/中止字样：" + ("无" if not bad else "；".join(bad[:3])))
print("   ⚠ 日志没有时间戳，打字窗口与 [MODE] 事件的对应关系判不出来"
      "（见 layering-plan.md P10.4）")
