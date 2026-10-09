#!/bin/bash
# 台架连续往返：主站 + 外部目标客户端，命令流全程保持在 200ms 失活阈值以内。
#
# 流向是最终场景那一条：客户端 -> 命令套接字 -> set_external_target ->
# 外部目标缓冲区 -> position_target_source -> CSP 目标。不启用部署里的内部
# 运动曲线，也不直接改过程映像。
#
# 必须在 root 下运行（主站需要 mlockall + SCHED_FIFO，套接字由 root 创建）。
# ulimit -r 由脚本自己提（见下方 set -u 之后），所以调用式可以就是一行、不带引号：
#   sudo DEPLOYMENT=orangepi-bench-non-30deg DURATION=60 TAG=r1 \
#        bash /home/orangepi/ethercat-master/scripts/bench_reciprocate_30deg.sh
# 要放后台就套 nohup：sudo nohup bash <脚本> >/dev/null 2>&1 &
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
#   REPORT_INTERVAL  客户端打印目标/实际的采样间隔（秒），默认 10。
#                    调成 1 能看出"轴从哪一刻起不再跟目标"，用来判断驱动器
#                    什么时候掉出 OP（客户端只发不跟 = 驱动器不再执行目标）。
#   ONLY_AXIS    只让第 N 根轴走（1 起，按 status 里的轴序），其余轴留在起点
#                原地保持；0 = 全部轴一起走（默认）。"有一根轴不跟"的排查用
#                这个把那一根单独拎出来——全动时它不动，可能是它自己的问题，
#                也可能是多轴一起时的别的问题；单独动它还不动，就只剩它自己。
#   SETTLE_TICKS 客户端结束后的保持窗口拍数（0.5 秒/拍），默认 10 拍 = 5 秒；
#                窗口内主站仍在跑、但已无命令流，用来分辨"掉出 OP 发生在命令流
#                运行期"还是"发生在这段保持窗口里"。设 0 = 不给窗口。
#                **窗口在发 shutdown 之前**：shutdown 会真的停机，摆在它后面窗口
#                就没了（P12.1 修好后如此）。
#
# 退出码：0 = 客户端跑满且命令流连续；1 = 命令流出现过 200ms 空档；2 = 主站中途
#         中断了命令流；3 = P12.1 端到端判据失败（命令口 shutdown 没让主站退出）。
#
# 报告路径取自部署配置的 run_report_path，无需另行指定。

set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# 构建目录跟随 CMakePresets.json 的 linux-debug（CI 也用同一个）。
# 单数形式 $REPO/build 是旧台架树的习惯，两处不一致会让脚本去找不存在的二进制。
BUILD=${BUILD:-$REPO/build/linux-debug}
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
DURATION=${DURATION:-600}
TRAVERSE=${TRAVERSE:-3}
DEGREES=${DEGREES:-30}
RATE=${RATE:-100}
WAVE=${WAVE:-triangle}
TAG=${TAG:-recip30}
REPORT_INTERVAL=${REPORT_INTERVAL:-10}
SETTLE_TICKS=${SETTLE_TICKS:-10}
# OBSERVATION 是否让主站开观测通道（1=开，默认 1）。
# 默认开着，是因为排查"轴掉出去"这件事**必须**有逐轴母线电压/温度/电流：慢速遥测
# 只在观测通道开着时才轮询，关着的话报告里那几栏恒为「—」，事后无法回答"掉的那一刻
# 电压是多少"（2026-09-29 的 nonall1 就是这么白跑一轮的）。
# 设 0 可对照"观测线程本身有没有干扰"，那是 P12.8 的 A/B 用法，不是默认用法。
OBSERVATION=${OBSERVATION:-1}
ONLY_AXIS=${ONLY_AXIS:-0}

SOCK=/tmp/emaster-${DEPLOYMENT}.sock
LOG=/tmp/${TAG}_master.log
OUT=/tmp/${TAG}_outcome.txt
DONE=/tmp/${TAG}_done
CLIENT_LOG=/tmp/${TAG}_client.log
STAMP=$(date '+%Y%m%d-%H%M%S')
# 归档挑选要用：只认这次运行开始之后才写出来的归档，见下面的收尾段。
START_EPOCH=$(date +%s)

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
    #
    # 必须真的用这个码退出。调用方取的是 $?，只 echo 不 exit 的话进程返回的是
    # echo 自己的状态 0 —— 于是"跑满(0)／有空档(1)／被主站中断(2)／shutdown 没停住(3)"
    # 四种结局在调用方眼里全是 0。bench_series.sh 把这个 $? 写进 index.tsv 的
    # exit_code 列，所以收尾掉轴那种轮次会显示成干干净净的一轮（2026-10-08 实测：
    # duodec25b 第 5 轮真值 2、索引里记成 0）。
    # 这一条只影响"跑起来之后"的四种结局；"主站没起来"那两条早退路径本来就直接 exit。
    exit "$1"
}

echo "=== 台架连续往返 $STAMP ==="
echo "部署=$DEPLOYMENT 时长=${DURATION}s 单程=${TRAVERSE}s 位移=${DEGREES}度 频率=${RATE}Hz 波形=$WAVE"
if [ "$ONLY_AXIS" != "0" ]; then
    echo "只动第 $ONLY_AXIS 轴，其余轴钉在起点原地保持"
fi
echo "主站日志=$LOG 客户端日志=$CLIENT_LOG"

# 清理上次遗留：先停主站，再删套接字，避免新主站绑到旧 inode 上
pkill -9 -f emaster-master 2>/dev/null
sleep 2
rm -f /tmp/emaster-*.sock

# OBS_PREFIX 为空时下面那行就是原样的命令行；为 "env EMASTER_OBSERVATION=1" 时
# 由 env 把开关递进主站的环境。不写成 "$OBS_PREFIX ./build/..." 是因为空字符串会
# 展开成一个空参数，主站会把它当成可执行文件名（P12.11 在 gui_e2e 上踩过这个）。
if [ "$OBSERVATION" = "1" ]; then
    env EMASTER_OBSERVATION=1 $BUILD/apps/master/emaster-master --deployment "$DEPLOYMENT" > "$LOG" 2>&1 &
else
    $BUILD/apps/master/emaster-master --deployment "$DEPLOYMENT" > "$LOG" 2>&1 &
fi
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
    --report-interval "$REPORT_INTERVAL" --only "$ONLY_AXIS" \
    > "$CLIENT_LOG" 2>&1
CLIENT_RC=$?
echo "客户端退出码=$CLIENT_RC（0=跑满时长且命令流连续，1=出现过 200ms 空档，2=主站中断了命令流）"
tail -6 "$CLIENT_LOG"

echo ""
echo "终点：$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | tr '|' '\n' | grep '^a' | tr '\n' ' ')"

# 停机：先给保持窗口，再从命令口发 shutdown，看主站是否自行退出。
#
# **保持窗口必须在 shutdown 之前**，这个顺序不是随手排的。窗口的作用是分辨
# "掉出 OP 发生在命令流运行期"还是"发生在这段保持窗口里"——命令流已经停了，
# 只有主站还在跑的这一段才能把两者分开。此前 shutdown 是个空操作（见下），
# 窗口摆在它后面、看起来一样能观察；P12.1 修好之后 shutdown 真的会停机，窗口
# 摆在它后面就会**无声地**缩成零长度，那种分辨能力跟着一起消失。
#
# P12.1 的端到端判据就在下面那段：shutdown 之后主站应当**自行退出**，走的是与
# SIGINT 完全相同的那条路。此前命令口的 stop/shutdown 只置报告字段
# report->stop_requested，不经过安全门（安全门读的是回调），于是它是一句空话：
# 报告里多一个 true，轴上什么都没发生，主站也不退。那时这段逻辑只能等超时、
# 然后把超时记成"已知缺陷"——等于把缺陷写成了期望。现在等不到就是**失败**。
if [ "$SETTLE_TICKS" -gt 0 ]; then
    echo "保持窗口：$SETTLE_TICKS 拍 × 0.5s（客户端已退出，主站仍在跑）"
    for _ in $(seq 1 "$SETTLE_TICKS"); do
        kill -0 "$MASTER_PID" 2>/dev/null || break
        sleep 0.5
    done
else
    echo "SETTLE_TICKS=0：不给保持窗口，客户端一退出就发 shutdown"
fi

STOP_RC=0
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "shutdown → 期望主站自行退出（生效延迟约 1 个周期 + 停机序言）"
    echo "shutdown" | timeout 3 nc -U "$SOCK" > /dev/null 2>&1
    SELF_EXIT=0
    for _ in $(seq 1 60); do
        if ! kill -0 "$MASTER_PID" 2>/dev/null; then SELF_EXIT=1; break; fi
        sleep 0.5
    done
    if [ "$SELF_EXIT" = "1" ]; then
        echo "命令口 shutdown 使主站自行退出（P12.1 通过）"
    else
        echo "FAIL：shutdown 后 30 秒主站仍在运行——P12.1 的缺陷回来了"
        STOP_RC=1
        kill -INT "$MASTER_PID" 2>/dev/null
        for _ in $(seq 1 60); do
            kill -0 "$MASTER_PID" 2>/dev/null || break
            sleep 0.5
        done
    fi
else
    echo "注意：保持窗口里主站已经不在了（掉出/崩溃），P12.1 这条判据本轮没跑到"
fi
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "SIGINT 后主站仍未退出，强制结束"
    pkill -9 -f emaster-master 2>/dev/null
    STOP_RC=1
fi

# 归档不在这里做：主站 publish 时会自己留一份历史副本（硬链接，名字是
# <报告主干>-<UTC>.json，保留份数由部署的 report_archive_keep 决定）。
# 脚本只把本次运行那一份找出来、把 TAG 补进名字。
#
# 两件事各自都不能省：
#   找"本次那一份"——老的写法是 cp "$REPORT"，只要报告存在就拷。主站启动失败
#   时 $REPORT 是上一轮留下的旧文件，于是这一轮会把上一轮的报告当成本轮产物
#   归档，时间和内容都对不上。改成按 mtime 过滤（严格晚于脚本启动）之后，
#   这种误认不可能发生。
#   补 TAG——同一部署的多次台架运行（冒烟/长跑/换参数）只靠 UTC 时间戳分不出
#   来是哪一个。TAG 加在时间戳之后，归档名的前缀仍是报告主干，主站的削旧照样
#   认得出它。
ARCHIVE_DIR=$(dirname "$REPORT")/archive
STEM=$(basename "$REPORT" .json)
FRESH=$(find "$ARCHIVE_DIR" -maxdepth 1 -name "${STEM}-*.json" \
    -newermt "@$START_EPOCH" -printf '%f\n' 2>/dev/null | sort | tail -1)
if [ -n "$FRESH" ]; then
    TAGGED="${FRESH%.json}-${TAG}.json"
    if mv "$ARCHIVE_DIR/$FRESH" "$ARCHIVE_DIR/$TAGGED" 2>/dev/null; then
        echo "报告归档：$ARCHIVE_DIR/$TAGGED ($(stat -c %s "$ARCHIVE_DIR/$TAGGED") 字节)"
    else
        echo "报告归档：$ARCHIVE_DIR/$FRESH（补 TAG 失败，用主站原名；文件本身没问题）"
    fi
elif [ -f "$REPORT" ]; then
    echo "警告：报告在但找不到本次运行的归档（$ARCHIVE_DIR/${STEM}-*.json），保留份数可能为 0"
else
    echo "警告：报告未生成：$REPORT"
fi

echo ""
echo "=== 命令流是否全程有效 ==="
# 判据是"转 HOLD 之后又切回 EXTERNAL"的次数，不是"日志里有没有 timeout"。
# 客户端退出后主站**必然**超时转 HOLD —— 保持窗口就是这么来的 —— 所以拿 timeout
# 当判据，只要 SETTLE_TICKS > 0 就每一轮都报"出现过空档"，与客户端退出码（0=连续）
# 直接打架，读的人只能靠自己判断该信哪一句。
# 切回 EXTERNAL 只在客户端重新发来目标时发生，所以 >1 次才是真的中断过。
RESUME_COUNT=$(grep -c "Switched to EXTERNAL" "$LOG" 2>/dev/null || true)
TIMEOUT_COUNT=$(grep -c "External target timeout" "$LOG" 2>/dev/null || true)
echo "切回 EXTERNAL ${RESUME_COUNT:-0} 次（1 = 只有开局那一次）｜转 HOLD ${TIMEOUT_COUNT:-0} 次（收尾窗口占 1 次属正常）"
if [ "${RESUME_COUNT:-0}" -gt 1 ]; then
    echo "结论：命令流中途中断过（转 HOLD 后又切回 EXTERNAL），以客户端退出码为准核对"
else
    echo "结论：无中途中断，命令流全程有效"
fi
echo "模式切换："
grep -n "\[MODE\]" "$LOG" | head -10

echo ""
echo "=== 周期与 WKC ==="
grep -n "WKC\|首次不符\|总线状态" "$LOG" | tail -15

echo ""
echo "=== 完成 $TAG ==="
# 停机那条判据单独占一个退出码：它与客户端的 0/1/2 是三件不同的事，混进同一个
# 数字里就没法从 _done 一眼看出"这一轮是跑得好还是停机坏了"。
if [ "$STOP_RC" != "0" ] && [ "$CLIENT_RC" = "0" ]; then
    echo "退出码 3：客户端那部分没问题，但命令口 shutdown 没能停住主站（P12.1）"
    finish 3
else
    finish "$CLIENT_RC"
fi
