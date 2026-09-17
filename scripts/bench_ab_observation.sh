#!/bin/bash
# 观测通道 / 命令通道的对抗负载 A/B：证明"外部客户端能把控制回路拖死"这件事，
# 以及修复之后的版本做不到。
#
# 每臂流程：清台架 -> 起主站 -> 等 OP -> 起对抗负载 -> 跑运动（或静默）-> 停机 -> 取指标。
# 判据是**周期是否还在推进**：用观测 socket 读环形缓冲的 cycle，不依赖主站日志的措辞。
# 1 kHz 下每臂的期望拍数是间隔秒数 ×1000，收尾静默 3 秒期是 3000 拍。
#
# 臂（ARMS 环境变量，空格分隔）：
#   base        无对抗负载、不开观测，作为对照
#   obs         观测客户端 50 Hz 正常拉取
#   obsstall    观测客户端拉一次后停住 16 秒（读侧慢）
#   obsnone     观测通道开着但一个客户端都没有（把"通道存在"与"客户端在场"分开）
#   noread_new  命令 socket 上"连上就不读"，跑当前构建
#   noread_old  同一条臂，跑对照二进制（见下）
#
# 用法（台架 root）：
#   ARMS='base noread_new noread_old' DUR=40 bash scripts/bench_ab_observation.sh
# 全部输出追加到 /tmp/ab_out.txt；每臂的主站日志另存 /tmp/ab_master_<标签>.log，
# 停机时的 AL 快照从后者里取——那是"周期被拖住"的实际代价所在。
#
# 对照二进制怎么来（OLD，默认 /tmp/emaster-old25-quint）：
#   "连上就不读"能冻住回路，靠的是阶段2.5 之前 respond 里的阻塞 write()。
#   所以对照件 = 当前源码反向打掉那一处：
#     git diff <阶段2.5提交> <阶段2.5提交>~1 -- src/bus/soem/command_server.c > /tmp/rev.patch
#     tar -c --exclude=./build --exclude=./.git --exclude=./runtime -C <仓库> . | tar -x -C /tmp/old_src
#     cd /tmp/old_src && patch -p1 < /tmp/rev.patch
#     cmake -S . -B build && cmake --build build -j4 --target emaster-master
#   那个提交只动了 command_server.c 一个文件、此后无人再动它，反向补丁必然干净。
#   不要用更早的提交整棵构建：那时的配置目录里还没有五轴部署。
#
# 2026-09-16 五轴实测（新旧交替 3 轮，三轮结论一致）：
#   noread_old  周期冻死，交换号停在原地 35 秒；停机时 5 个轴全部 AL state=20 (0x001A) 掉出 OP
#   noread_new  周期按 97% 额定推进（32046 / 期望 32000），停机时 5 个轴全部 AL state=8
#               对抗客户端在第 258 组被主站主动断开（Broken pipe）——这正是设计的处置方式
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPLOY=${DEPLOY:-orangepi-bench-quint-30deg}
SOCK=/tmp/emaster-${DEPLOY}.sock
OSOCK=/tmp/emaster-${DEPLOY}-obs.sock
REPORT_REL=$(DEPLOYMENT="$DEPLOY" python3 -c "
import json, os, glob, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob('$REPO/config/deployments/*.json')):
    with open(path) as handle:
        if json.load(handle).get('deployment_id') == target:
            print(json.load(open(path))['run_report_path']); sys.exit(0)
sys.exit(1)
" 2>/dev/null)
if [ -z "$REPORT_REL" ]; then echo "错误：读不出部署 $DEPLOY 的 run_report_path" >&2; exit 1; fi
REPORT=$REPO/$REPORT_REL
NEW=$REPO/build/tools/master/emaster-master
OLD=${OLD:-/tmp/emaster-old25-quint}
DUR=${DUR:-40}
OUT=${OUT:-/tmp/ab_out.txt}
# 同标签的臂会互相覆盖日志，加个序号，失败那一轮才留得下来。
SEQ=0

# 自己要先写的临时文件，一律先删后建。这不是洁癖，是 2026-09-17 踩出来的：
#
# /tmp 是 1777 sticky，而 fs.protected_regular=2（Ubuntu 默认）**连 root 也拦**——
# 目录里已存在的、属主不是自己的文件，root 打不开来写（报"权限不够"）。只要有过
# 一次以别的用户跑起来，这些文件就归了那个用户，之后每次以 root 跑都会：
#
#   1. `: > "$OUT"` 失败 → 整个臂的输出丢失（exec 重定向会直接让脚本退出）；
#   2. `echo $$ > /tmp/ab.pid` 静默失败 → ab.pid 停在旧值 → `kill -INT` 打空 →
#      下面的等待循环立刻判定"主站已自行退出"，而主站其实还在跑，最后死于
#      下一臂的 `pkill -9`：**停机路径和报告一起丢，驱动器停在使能态**；
#   3. 指标那一段 grep 的是上一轮留下的旧报告，打出来的数字看着像结果。
#
# 第 3 条最贵：它不报错，只让一整臂的数据变成上一臂的，且只有逐字段比对才发现
# 得了。删掉重建即可——sticky 位只挡普通用户，root 能 unlink 任何东西。
rm -f "$OUT" /tmp/ab.pid /tmp/ab_master.log /tmp/ab_master_*.log \
      /tmp/ab_noread.py /tmp/ab_adv.log /tmp/ab_motion.log 2>/dev/null

: > "$OUT"
exec >>"$OUT" 2>&1

cat > /tmp/ab_noread.py <<'PY'
import socket, sys, time
sockpath, seconds, traverse, degrees, rate, cpd = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6])
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(sockpath)
# 起手先读一次 status 拿**当前实际位置**当起点。"不读响应"这条臂针对的是持续命令流，
# 起手读一次不改变它的性质；反过来，把起点写死会在轴停在别处时触发单步限幅，
# 主站直接故障停机——那样测到的就不是背压，而是我自己的客户端在乱报目标。
sock.sendall(b"status\n")
buf = b""
while b"\n" not in buf:
    chunk = sock.recv(4096)
    if not chunk:
        break
    buf += chunk
start = []
for part in buf.decode(errors="replace").split("|"):
    if part.startswith("a") and ":pos=" in part:
        start.append(int(part.split(":pos=")[1].split(",")[0]))
if not start:
    print("拿不到起点，status 回包前 200 字节：", buf[:200], flush=True)
    sys.exit(2)
print(f"起点（实读，{len(start)} 轴）=", start, flush=True)
period = 1.0 / rate
delta = [int(round(degrees * cpd))] * len(start)
t0 = time.time(); sent = 0; next_t = t0
while time.time() - t0 < seconds:
    now = time.time()
    ph = ((now - t0) % (2 * traverse)) / traverse
    if ph > 1.0: ph = 2.0 - ph
    frame = [start[i] + int(round(delta[i] * ph)) for i in range(len(start))]
    try:
        sock.sendall(b"set_external_target " + " ".join(str(v) for v in frame).encode() + b"\n")
        sock.sendall(b"status\n")
    except OSError as exc:
        print(f"客户端在第 {sent} 组时被断开：{exc}", flush=True); break
    sent += 1
    next_t += period
    gap = next_t - time.time()
    if gap > 0: time.sleep(gap)
    else: next_t = time.time()
print(f"发出 {sent} 组（各一条 set_external_target + status），全程未读响应。", flush=True)
time.sleep(3)
PY

probe_cycle() {
    timeout 3 python3 tools/observation_client.py --socket "$OSOCK" LATEST 2>/dev/null \
        | grep -o 'cycle=[0-9]*' | head -1 | cut -d= -f2
}

start_master() {
    export EMASTER_OBSERVATION=$1
    setsid bash -c "echo \$\$ > /tmp/ab.pid; cd $REPO; exec $2 --deployment $DEPLOY" > /tmp/ab_master.log 2>&1 &
    # ab.pid 必须真的写进去了才能往下走。写不进去时后面 `kill -INT` 会打空，而
    # 打空的表现是"主站已自行退出"——一个看起来完全正常的假象，代价是整臂的停机
    # 路径和报告（见文件头 rm -f 那段）。宁可在这里硬失败。
    for _ in $(seq 1 20); do [ -s /tmp/ab.pid ] && break; sleep 0.1; done
    if ! grep -qE '^[0-9]+$' /tmp/ab.pid 2>/dev/null; then
        echo "** 错误：/tmp/ab.pid 没写进去或不是 PID（$(cat /tmp/ab.pid 2>&1)），停机将失去目标" >&2
        return 1
    fi
    for _ in $(seq 1 60); do [ -S "$SOCK" ] && break; sleep 0.5; done
    local st=0
    for _ in $(seq 1 60); do
        st=$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null | grep -o 'state=[0-9]*' | head -1 | cut -d= -f2)
        [ "$st" = "4" ] && break
        sleep 1
    done
    # 只往 stdout 写状态值：调用方用 $(...) 接它，多打一个字就会让比较失败
    printf '%s' "$st"
}

clean() {
    pkill -9 -f emaster-master 2>/dev/null
    for _ in $(seq 1 10); do pgrep -f 'tools/master/emaster-master' >/dev/null || break; sleep 1; done
    rm -f /tmp/emaster-*.sock
}

# 报告是不是本臂写出来的。判据只看 mtime 有没有前进，不看内容——
# 报告写不出来时，`grep "$REPORT"` 照样有输出，只是那些数字全是上一轮的。
# 2026-09-17 正是这样把一整臂的指标读成了上一臂的，而且逐字段都和上臂一模一样
# 才被看出来。读取 RPT_BEFORE（arm() 里的 local，bash 动态作用域可见）。
report_is_fresh() {
    if [ ! -f "$REPORT" ]; then
        echo "** 报告不存在：$REPORT"
        return 1
    fi
    local now; now=$(stat -c %Y "$REPORT" 2>/dev/null)
    if [ "$now" = "${RPT_BEFORE:-}" ]; then
        echo "** 报告未更新（mtime 与本臂开跑前相同）：本臂的停机/报告路径没走到，"
        echo "** 下面若还有指标，那是上一轮的旧数据，本臂无效。"
        return 1
    fi
    return 0
}

# 关心的字段。每次都从同一处取，免得三个分支各写一份、加字段时漏掉其中一个。
metric_grep() {
    grep -oE '"(frame_interval_max_ns|frame_interval_gap_count|deadline_missed_count|wkc_mismatch_count|wkc_no_frame_count|publish_max_ns|publish_over_budget_count|frame_timeout_us|tail_max_receive_ns|tail_max_receive_ok_ns|sync0_late_count)": *[0-9]+' \
        "$REPORT" | sort -u
}

arm() {   # $1=标签 $2=二进制 $3=观测开关 $4=对抗(none|watch|stall|noread)
    local TAG=$1 BIN=$2 OBSV=$3 ADV=$4
    SEQ=$((SEQ+1))
    local STORE="/tmp/ab_master_${SEQ}_${TAG}.log"
    local RPT_BEFORE=""
    [ -f "$REPORT" ] && RPT_BEFORE=$(stat -c %Y "$REPORT" 2>/dev/null)
    # 对抗负载占了观测 socket 时不能在里面探测：观测 socket 是单客户端模型
    # （server.c 的"接管新连接前先断开旧的"），探测开的每一条新连接都会把对抗
    # 客户端顶掉。那样这条臂测到的"客户端被断开"来自我自己的探针，而不是背压，
    # 整条臂就白跑了——第一版正是这样。
    # noread 臂不受影响：它占的是命令 socket，观测 socket 空着。
    local ADV_OBS=0
    case "$ADV" in watch|stall) ADV_OBS=1 ;; esac
    clean
    echo ""
    echo "################ 臂 $TAG  观测=$OBSV 对抗=$ADV  $(md5sum "$BIN" | cut -c1-8) ################"
    echo "本臂主站日志=$STORE"
    cd "$REPO" || return 1
    ST=$(start_master "$OBSV" "$BIN")
    echo "主站 state=$ST"
    if [ "$ST" != "4" ]; then tail -5 /tmp/ab_master.log; return 1; fi
    local MPID; MPID=$(cat /tmp/ab.pid)
    local T0; T0=$(date +%s%N)
    local C0; C0=$(probe_cycle)
    echo "起始 cycle=$C0"

    local APID=""
    case "$ADV" in
        watch) timeout $((DUR+10)) python3 -u tools/observation_client.py --socket "$OSOCK" WATCH --hz 50 --seconds $((DUR-2)) > /tmp/ab_adv.log 2>&1 & APID=$! ;;
        stall) timeout $((DUR+10)) python3 -u tools/observation_client.py --socket "$OSOCK" --stall 16 > /tmp/ab_adv.log 2>&1 & APID=$! ;;
        noread) python3 -u /tmp/ab_noread.py "$SOCK" $((DUR+2)) 3 30 100 1274.31 > /tmp/ab_adv.log 2>&1 & APID=$! ;;
    esac
    sleep 3
    local C1=""
    [ "$ADV_OBS" = "1" ] || C1=$(probe_cycle)

    if [ "$ADV" = "noread" ]; then
        sleep $((DUR-8))
    else
        python3 -u scripts/test_external_motion_client.py "$SOCK" \
            --cycles 0 --total "$DUR" --traverse 3 --degrees 30 \
            --rate 100 --wave triangle --ready 30 > /tmp/ab_motion.log 2>&1
        echo "运动客户端退出码=$?  $(grep -c '空档' /tmp/ab_motion.log) 处空档提示"
        tail -3 /tmp/ab_motion.log
    fi

    local C2=""
    [ "$ADV_OBS" = "1" ] || C2=$(probe_cycle)
    [ -n "$APID" ] && { kill $APID 2>/dev/null; wait $APID 2>/dev/null; echo "--- 对抗侧："; tail -4 /tmp/ab_adv.log; }
    sleep 3
    local C3; C3=$(probe_cycle)
    local T1; T1=$(date +%s%N)

    # 停机必须在这里、即在 ADV_OBS 提前返回之前：停机走 SIGINT（安全门），
    # 下一臂的 clean() 走的是 pkill -9。漏掉这一步，主站会被 SIGKILL 掉——
    # 驱动器停在被使能、输出还压着的状态，下一臂一起就带着上臂的残留跑。
    # 第一版的 ADV_OBS 分支把 return 0 写在了这一段前面，第三臂因此一开机
    # 就丢轴掉出 OP，那次结果不能算数。
    # 发信号前先确认 MPID 指的确实是主站。ab.pid 指向别的 PID（或进程已消失）时
    # `kill -0` 会立刻失败，被下面的循环读成"主站已自行退出"——主站其实还在跑，
    # 最后死于下一臂的 pkill -9：停机路径和报告一起丢，驱动器停在使能态。
    # 所以不认文件，按进程名兜底重解析一次。
    if ! tr '\0' ' ' < "/proc/$MPID/cmdline" 2>/dev/null | grep -q emaster-master; then
        local REAL; REAL=$(pgrep -f 'tools/master/emaster-master' | head -1)
        echo "** ab.pid=$MPID 不是主站或已消失，按进程名重解析为 ${REAL:-无}"
        MPID=${REAL:-$MPID}
    fi

    kill -INT "$MPID" 2>/dev/null
    local OK=0
    for _ in $(seq 1 120); do kill -0 "$MPID" 2>/dev/null || { OK=1; break; }; sleep 0.5; done
    if [ "$OK" = "1" ]; then echo "主站已自行退出"
    else echo "** 主站 60s 内未退出"; pkill -9 -f emaster-master; fi

    if [ "$ADV_OBS" = "1" ]; then
        # 对抗客户端在场时全程不探测，只比较首尾，期望值用实测墙钟换算
        # （1 kHz 下拍数就是毫秒数），不再假设各段各占多久。
        local WANT=$(( (T1-T0)/1000000 ))
        local GOT=$((C3-C0))
        echo "cycle: 起=$C0 结束后=$C3（对抗客户端在场，中间不探测）"
        echo "--- 判定（实测间隔 $((WANT/1000)) 秒，期望 $WANT 拍）---"
        if [ "$GOT" -ge $((WANT*95/100)) ] && [ "$GOT" -le $((WANT*105/100)) ]; then
            echo "结论：周期未受干扰（推进 $GOT）"
        elif [ "$GOT" -lt $((WANT/10)) ]; then
            echo "** 结论：周期被冻死（推进 $GOT）"
        else
            echo "** 结论：周期被拖慢（推进 $GOT）"
        fi
        echo "--- 指标 ---"
        report_is_fresh && metric_grep
        cp /tmp/ab_master.log "$STORE" 2>/dev/null || true
        echo "--- 停机 AL 快照 ---"
        grep -E 'AL state=' "$STORE" | tail -8
        grep -E '总线状态' /tmp/ab_master.log | tail -1 | cut -c1-160
        return 0
    fi

    echo "cycle: 起=$C0 运动初=$C1 运动末=$C2 结束后=$C3"
    if [ -z "$C3" ]; then
        # base 臂不开观测通道，探针自然读不到 cycle。此时唯一有效的判据是下面的
        # 停机 AL 快照：轴有没有掉出 OP。不写这段保护的话，空字符串参与算术
        # 会让 bash 报语法错，看着像脚本坏了。
        echo "（本臂无观测通道，跳过推进判定；看下面的停机 AL 快照）"
        echo "--- 指标 ---"
        report_is_fresh && metric_grep
        cp /tmp/ab_master.log "$STORE" 2>/dev/null || true
        echo "--- 停机 AL 快照 ---"
        grep -E 'AL state=' "$STORE" | tail -8
        grep -E '首次不符|总线状态' /tmp/ab_master.log | tail -2 | cut -c1-160
        return 0
    fi
    # 判定按每臂的实际间隔算：noread 臂只 sleep DUR-8 就拿 C2，
    # 拿"1 kHz × DUR"当期望值会把它判成被拖住（第一版正是这么错的）。
    local SPAN=$((DUR-8))
    [ "$ADV" = "noread" ] || SPAN=$DUR
    local WANT=$((SPAN*1000))
    echo "--- 判定（本臂期望 $WANT 拍，静默期期望 3000 拍）---"
    if [ $((C2-C1)) -ge $((WANT*95/100)) ] && [ $((C3-C2)) -ge 2500 ]; then
        echo "结论：周期未受干扰（运动期推进 $((C2-C1))，静默期 3s 推进 $((C3-C2))）"
    elif [ $((C2-C1)) -lt $((WANT/10)) ]; then
        echo "** 结论：周期被冻死（运动期推进 $((C2-C1))，静默期 3s 推进 $((C3-C2))）"
    else
        echo "** 结论：周期被拖慢（运动期推进 $((C2-C1))，静默期 3s 推进 $((C3-C2))）"
    fi

    echo "--- 指标 ---"
    if report_is_fresh; then
        metric_grep
    else
        echo "（本臂报告不可用，见上；不要用上面任何数字）"
    fi
    cp /tmp/ab_master.log "$STORE" 2>/dev/null || true
    echo "--- 停机 AL 快照 ---"
    grep -E 'AL state=' "$STORE" | tail -8
    echo "--- 主站日志关键行 ---"
    grep -cE 'External target timeout' /tmp/ab_master.log
    grep -E '总线状态' /tmp/ab_master.log | tail -1 | cut -c1-160
}

for A in ${ARMS:-base obs obsstall noread_new noread_old}; do
    case "$A" in
        base)        arm base       "$NEW" 0 none ;;
        obsnone)     arm obsnone    "$NEW" 1 none ;;
        obs)         arm obs        "$NEW" 1 watch ;;
        obsstall)    arm obsstall   "$NEW" 1 stall ;;
        noread_new)  arm noread_new "$NEW" 1 noread ;;
        noread_old)  arm noread_old "$OLD" 1 noread ;;
        *) echo "未知臂 $A" ;;
    esac
done
echo ""
echo "################ 全部结束 ################"
