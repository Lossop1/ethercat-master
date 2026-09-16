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

arm() {   # $1=标签 $2=二进制 $3=观测开关 $4=对抗(none|watch|stall|noread)
    local TAG=$1 BIN=$2 OBSV=$3 ADV=$4
    clean
    echo ""
    echo "################ 臂 $TAG  观测=$OBSV 对抗=$ADV  $(md5sum "$BIN" | cut -c1-8) ################"
    cd "$REPO" || return 1
    ST=$(start_master "$OBSV" "$BIN")
    echo "主站 state=$ST"
    if [ "$ST" != "4" ]; then tail -5 /tmp/ab_master.log; return 1; fi
    local MPID; MPID=$(cat /tmp/ab.pid)
    local C0; C0=$(probe_cycle)
    echo "起始 cycle=$C0"

    local APID=""
    case "$ADV" in
        watch) timeout $((DUR+10)) python3 -u tools/observation_client.py --socket "$OSOCK" WATCH --hz 50 --seconds $((DUR-2)) > /tmp/ab_adv.log 2>&1 & APID=$! ;;
        stall) timeout $((DUR+10)) python3 -u tools/observation_client.py --socket "$OSOCK" --stall 16 > /tmp/ab_adv.log 2>&1 & APID=$! ;;
        noread) python3 -u /tmp/ab_noread.py "$SOCK" $((DUR+2)) 3 30 100 1274.31 > /tmp/ab_adv.log 2>&1 & APID=$! ;;
    esac
    sleep 3
    local C1; C1=$(probe_cycle)

    if [ "$ADV" = "noread" ]; then
        sleep $((DUR-8))
    else
        python3 -u scripts/test_external_motion_client.py "$SOCK" \
            --cycles 0 --total "$DUR" --traverse 3 --degrees 30 \
            --rate 100 --wave triangle --ready 30 > /tmp/ab_motion.log 2>&1
        echo "运动客户端退出码=$?  $(grep -c '空档' /tmp/ab_motion.log) 处空档提示"
        tail -3 /tmp/ab_motion.log
    fi

    local C2; C2=$(probe_cycle)
    sleep 3
    local C3; C3=$(probe_cycle)
    echo "cycle: 起=$C0 运动初=$C1 运动末=$C2 结束后=$C3"
    [ -n "$APID" ] && { kill $APID 2>/dev/null; wait $APID 2>/dev/null; echo "--- 对抗侧："; tail -4 /tmp/ab_adv.log; }
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

    kill -INT "$MPID" 2>/dev/null
    local OK=0
    for _ in $(seq 1 60); do kill -0 "$MPID" 2>/dev/null || { OK=1; break; }; sleep 0.5; done
    if [ "$OK" = "1" ]; then echo "主站已自行退出"; else echo "** 主站 30s 内未退出"; pkill -9 -f emaster-master; fi
    echo "--- 指标 ---"
    if [ -f "$REPORT" ]; then
        grep -oE '"(frame_interval_max_ns|frame_interval_gap_count|deadline_missed_count|wkc_mismatch_count|wkc_no_frame_count|publish_max_ns|publish_over_budget_count)": *[0-9]+' "$REPORT" | sort -u
    else
        echo "（无报告）"
    fi
    cp /tmp/ab_master.log "/tmp/ab_master_${TAG}.log" 2>/dev/null || true
    echo "--- 停机 AL 快照 ---"
    grep -E 'AL state=' "/tmp/ab_master_${TAG}.log" | tail -8
    echo "--- 主站日志关键行 ---"
    grep -cE 'External target timeout' /tmp/ab_master.log
    grep -E '总线状态' /tmp/ab_master.log | tail -1 | cut -c1-160
}

for A in ${ARMS:-base obs obsstall noread_new noread_old}; do
    case "$A" in
        base)        arm base       "$NEW" 0 none ;;
        obs)         arm obs        "$NEW" 1 watch ;;
        obsstall)    arm obsstall   "$NEW" 1 stall ;;
        noread_new)  arm noread_new "$NEW" 1 noread ;;
        noread_old)  arm noread_old "$OLD" 1 noread ;;
        *) echo "未知臂 $A" ;;
    esac
done
echo ""
echo "################ 全部结束 ################"
