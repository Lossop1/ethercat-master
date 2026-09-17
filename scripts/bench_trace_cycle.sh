#!/bin/bash
# 周期线程的"谁拖慢了这一拍"取证配方：跑一轮台架运行，同时用 ftrace 把
# 用户态探针 + 调度/中断 + 系统调用记下来。
#
# 要回答的问题是两种完全不同的可能：
#   1) 主站自己慢 —— 这段活本来就要那么久；
#   2) 线程被拿走 —— 活不多，但 CPU 不在它手里。
# 两者的时间线长得不一样，所以记录里同时要有"它在跑"和"它没在跑"两面的证据。
#
# 记三样东西：
#   a) uprobe：四个用户态函数入口各一个点，把周期拆成几段。
#        交换入口 → 计时统计入口（= 发/收/邮箱/计数器四段）
#        计时统计 → PDO 审计输出（= 现场入环那一段）
#        审计输出 → 下一个交换入口（= 探针 + WKC 策略 + 截止期判定 + 收尾）
#      哪一段出问题，看记录里两点的间隔就知道，不用改代码加计时点。
#      注册用的文件偏移由 scripts/analysis/uprobe_offsets.py 从 ELF 换算
#      （uprobe 要的是文件偏移，`nm` 给的是虚拟地址，段对齐不齐时两者不等）。
#   b) sched_switch / sched_waking —— 线程什么时候在 CPU 上、什么时候被换下去。
#   c) irq_handler_* / softirq_* / raw_syscalls —— 抢占它的是硬中断、软中断，
#      还是它自己发起的系统调用。clock_gettime 在 arm64 上走 vDSO，本来不该
#      出现系统调用；它要是出现了，本身就是结论。
#
# 跑完用 scripts/analysis/trace_busy_windows.py 读结果：把"一次连续占用 CPU
# 超过 N ms"的窗口摊开，事件带人话标签。
#
# 参数（环境变量）：
#   TAG       本轮标签，决定 /tmp/<TAG>_trace.txt 等文件名，默认 trace
#   DURATION  运行秒数，默认 180
#   DEGREES   往复幅度，默认 0（不动轴）。
#             不动轴是故意的：证据指向"停摆结束运行"，不是"运行结束触发停摆"，
#             所以轴转不转与停摆无关；不转省电、不发热，5 号从站也不用受热。
#   CPU       只抓哪一核，默认 11。**必须与主站部署里周期线程的 RT 绑定一致**，
#             否则记录里没有周期线程。
#   BUFFER_MB 给这一核多大的环，默认 2048（按本台架实测事件密度约 4.7 MB/s，
#             约装 7 分钟；要更长就调大）。
#
# 需要 root：要在 tracefs 里注册探针、改缓冲。用法见 scripts/analysis/ 下的
# 两个脚本头部。
set -u

T=/sys/kernel/debug/tracing
REPO=/home/orangepi/ethercat-master
BIN=$REPO/build/tools/master/emaster-master
TAG=${TAG:-trace}
TRACE=/tmp/${TAG}_trace.txt
RUNLOG=/tmp/${TAG}_run.txt
OFF=/tmp/${TAG}_off.txt
DURATION=${DURATION:-180}
DEGREES=${DEGREES:-0}
CPU=${CPU:-11}
BUFFER_MB=${BUFFER_MB:-2048}

rm -f /tmp/${TAG}_done /tmp/${TAG}_rc "$TRACE"
: > "$RUNLOG"

fail() {
    echo "失败：$1" | tee -a "$RUNLOG"
    echo 1 > /tmp/${TAG}_rc
    touch /tmp/${TAG}_done
    exit 1
}

echo "== 开记录 $(date '+%F %T') ==" >> "$RUNLOG"
echo 0 > "$T/tracing_on"

# 上一轮留下的探针要先摘掉，否则重名会让这次的注册失败。
echo > "$T/uprobe_events" 2>/dev/null

python3 "$REPO/scripts/analysis/uprobe_offsets.py" "$BIN" \
    emaster_soem_session_exchange \
    emaster_cyclic_timing_stats_record \
    emaster_cia_process_image_audit_output \
    emaster_command_server_receive > "$OFF" 2>&1 || fail "符号换算脚本没跑起来"
cat "$OFF" >> "$RUNLOG"
grep -q MISSING "$OFF" && fail "有符号没找到（见 $OFF）"

offset_of() { awk -v n="$1" '$1 == n { print $2 }' "$OFF"; }
EXCH=$(offset_of emaster_soem_session_exchange)
TSTAT=$(offset_of emaster_cyclic_timing_stats_record)
AUDIT=$(offset_of emaster_cia_process_image_audit_output)
CMDRECV=$(offset_of emaster_command_server_receive)
for pair in "EXCH:$EXCH" "TSTAT:$TSTAT" "AUDIT:$AUDIT" "CMDRECV:$CMDRECV"; do
    [ -n "${pair#*:}" ] || fail "偏移为空：$pair"
done

echo "p:cyc_exch  $BIN:$EXCH"  >> "$T/uprobe_events"
echo "p:cyc_tstat $BIN:$TSTAT" >> "$T/uprobe_events"
echo "p:cyc_audit $BIN:$AUDIT" >> "$T/uprobe_events"
echo "p:cyc_cmd   $BIN:$CMDRECV" >> "$T/uprobe_events"
{
    echo "== 注册的探针 =="
    cat "$T/uprobe_events"
} >> "$RUNLOG" 2>&1

for group in cyc_exch cyc_tstat cyc_audit cyc_cmd; do
    [ -e "$T/events/uprobes/$group/enable" ] || fail "探针 $group 没建出来（见 $RUNLOG）"
    echo 1 > "$T/events/uprobes/$group/enable"
done

for event in \
    sched/sched_switch sched/sched_waking \
    irq/irq_handler_entry irq/irq_handler_exit \
    irq/softirq_entry irq/softirq_exit \
    raw_syscalls/sys_enter raw_syscalls/sys_exit; do
    [ -e "$T/events/$event/enable" ] || fail "事件 $event 不存在"
    echo 1 > "$T/events/$event/enable"
done

: > "$T/trace"
echo mono > "$T/trace_clock"

# 只抓周期线程那一核。写死这一句是有意的：tracefs 的设置会一直留着，不显式设就是
# 在赌上一次运行留下的值正好对。
printf '%x' $((1 << CPU)) > "$T/tracing_cpumask"

# buffer_size_kb 是**每 CPU 一份**，整机占用 = 本值 × 核数（`buffer_total_size_kb` 就是
# 乘出来的那个数）。所以"能给多大"由这台机器的内存决定，**没有一个固定的内核上限**：
# 本台架（12 核 / 32 GB）写 1 GB/核 成功、2 GB/核 也成功，只有写 2 GB/核 **×12** 才 ENOMEM。
# 既然只抓一核，就没必要给 12 份都加大——老写法往全局文件写，在 12 核上等于把内存乘 12 倍
# （实测一次 1 GB/核 的全局写吃掉 12 GB）。这里只改抓的那一核，写不进去再退回全局。
# 写不进去不是致命错误，但要在日志里留下"实际是多少"，免得事后误判（全局文件在
# 各核不一致时会读成 `X`，看到 `X` 属正常）。
if ! echo $((BUFFER_MB * 1024)) > "$T/per_cpu/cpu$CPU/buffer_size_kb" 2>/dev/null; then
    if ! echo $((BUFFER_MB * 1024)) > "$T/buffer_size_kb" 2>/dev/null; then
        echo "缓冲写入失败（$?），沿用当前值" >> "$RUNLOG"
    fi
fi
{
    echo "== 配置 =="
    echo -n "tracing_cpumask="; cat "$T/tracing_cpumask"
    echo -n "cpu$CPU/buffer_size_kb="; cat "$T/per_cpu/cpu$CPU/buffer_size_kb"
    echo -n "buffer_size_kb="; cat "$T/buffer_size_kb"
    echo -n "buffer_total_size_kb="; cat "$T/buffer_total_size_kb"
    echo -n "trace_clock="; cat "$T/trace_clock"
    echo "TAG=$TAG DURATION=$DURATION DEGREES=$DEGREES CPU=$CPU BUFFER_MB=$BUFFER_MB"
} >> "$RUNLOG"
echo 1 > "$T/tracing_on"

cd "$REPO" || fail "进不去仓库目录"
DURATION="$DURATION" DEGREES="$DEGREES" TRAVERSE=3 RATE=100 WAVE=triangle TAG="$TAG" \
    bash scripts/bench_reciprocate_30deg.sh
echo "$?" > /tmp/${TAG}_rc

echo 0 > "$T/tracing_on"
cat "$T/trace" > "$TRACE"
for group in cyc_exch cyc_tstat cyc_audit cyc_cmd; do
    echo 0 > "$T/events/uprobes/$group/enable"
done
echo > "$T/uprobe_events" 2>/dev/null

{
    echo "== 收记录 $(date '+%F %T') =="
    echo -n "退出码="; cat /tmp/${TAG}_rc
    echo -n "记录行数="; wc -l < "$TRACE"
    echo -n "记录字节="; stat -c %s "$TRACE"
    echo -n "丢弃事件行="; grep -c "events dropped" "$TRACE" || true
    echo -n "uprobe 命中 cyc_exch="; grep -c "cyc_exch:" "$TRACE" || true
    echo -n "uprobe 命中 cyc_tstat="; grep -c "cyc_tstat:" "$TRACE" || true
    echo -n "uprobe 命中 cyc_audit="; grep -c "cyc_audit:" "$TRACE" || true
    echo -n "uprobe 命中 cyc_cmd="; grep -c "cyc_cmd:" "$TRACE" || true
    echo -n "raw_syscalls 行数="; grep -c "sys_enter\|sys_exit" "$TRACE" || true
} >> "$RUNLOG" 2>&1
touch /tmp/${TAG}_done
