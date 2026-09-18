#!/bin/bash
# 台架：界面这一层跑在真主站上，一次端到端的控制 + 监控验证，并且**量出它抢不抢资源**。
#
# 跑法（香橙派上，root）：
#   sudo bash scripts/bench_gui_e2e.sh
#   sudo ARMS='solo bridge obs' ROUNDS=2 DUR=60 bash scripts/bench_gui_e2e.sh
#
# 臂（ARMS 环境变量，空格分隔）：
#   solo     主站自己跑，不起桥、不起界面 —— 基线
#   bridge   主站 + 桥，没有界面 —— 把"桥的开销"和"界面的开销"分开
#   obs      主站 + 桥 + **只读**界面（无头）。这是"主站开着就开着监控"那条纪律的兑现
#   drive    主站 + 桥 + 界面**接管并走经典往返**（全轴同时 30°、每趟 3 秒）——
#            会让轴真动，默认不跑（见下）。幅度/时长可用 RECIP_DEG / RECIP_TRAVERSE 改
#
# 判据是 A/B 的：每个非 solo 臂跟同轮 solo 臂比报告里的**两个计数**
#   deadline_missed_count / frame_interval_gap_count
# 以及主站日志里的掉出 OP。**结论要写成"不劣于基线"，不是"零错"**——基线自己有多少
# 错是台架当时的状况，不是这一层能决定的。
#
# frame_interval_max_ns 也打出来，但**不参与判定**：它是极值，差个几十微秒是台架逐轮
# 的正常抖动。拿 `>` 去比它，每轮都会报"劣于基线"，那种报警读两次就没人看了。
# 要判断极值有没有真的变坏，看那一列的数，别让脚本替你下结论。
#
# ---------------------------------------------------------------------------
# 观测开关：**主站默认不建观测套接字**，得由起它的人把 EMASTER_OBSERVATION 打开
#
# 2026-09-18 那次跑，四个臂全绿、A/B 三项全 0、五轴 30° 往返每一趟都走到了，而
# **监控半边一帧都没收到**：主站压根没建 /tmp/emaster-<部署>-obs.sock，界面的取数
# 线程每一轮都连不上，报告里那句"观测：一帧都没收到"就躺在输出里没人判。原因就是
# 这个脚本起主站时没设那个环境变量——观测是主站的**旁路**，开关关掉时它连缓冲都不
# 分配，而且只在报告里自证（report.observation.enabled=false）。
#
# 所以现在：需要监控的臂（bridge/obs/drive）由脚本把开关打开，并且**开局就查套接字
# 在不在**，收尾还要拿 tools/gui_evidence.py 判一次"这一臂的监控到底成没成"。
# 判红就明说这一臂不算通过——打印事实的地方不负责说这算不算数，这句话上一轮已经
# 付过学费了。
#
# 这段接线本身怎么验：**不用上硬件**，拿 scripts/bench_gui_e2e_stub.py 当主站干跑一遍
# （它只在 EMASTER_OBSERVATION=1 时才建观测套接字，所以"开关递到没有"真的会被判）。
# 命令与注意事见那个文件的头部注释。台架上次是整场跑完才发现监控半边是空的，
# 那种发现方式太晚了。
#
# ---------------------------------------------------------------------------
# 三条不能省的前提
#
# **这一层不许和主站抢资源（用户定的）。** 所以这里不印"看起来没影响"就完事：桥和界面
# 各自绑到主站用不着的核上（rt_affinity 自己从 /proc 读主站三个实时线程的掩码），
# 而且 A/B 真的把差距量出来。抢资源这件事没有"理论上不会"。
#
# **drive 臂会让轴真动，而且是 30° 一档，不是点一下。** 涉及物理动作先确认，所以它
# 不默认跑，而且光写进 ARMS 还不够，还要一个显式的 CONFIRM_MOTION=yes（见下面那道拦截）：
#   sudo ARMS='solo obs drive' CONFIRM_MOTION=yes bash scripts/bench_gui_e2e.sh
# 跑之前先确认轴周围没人没夹具、30° 行程有净空，并给 5 号从站（小电机，会烫）留冷却间隔。
#
# **跑完立刻停机。** 每臂结束都 SIGINT 主站并等报告写完。不是 pkill：SIGINT 走的是
# 安全门那条有序停机路径，报告才写得完整；pkill -9 会把驱动器留在使能态。
#
# ---------------------------------------------------------------------------
# 为什么桥和界面都连 127.0.0.1（不跨机器）
#
# 这一条是**有意的**，不是将就。桥的 `--bind` 默认就是回环，跨机器要显式填管理口 IP；
# 而那两个口**没有任何认证**，命令口能驱动电机。跨机器那条路是给人坐在 Windows 前
# 面用的（手动验证、看着曲线点按钮），不该把它搭进一个自动跑的取证脚本里——那等于
# 每跑一次就多开一次局域网暴露面。所以：**自动取证走回环，跨机器手动来。**
#
# 2026-09-18 说明：本脚本已经在真主站上跑过一轮（solo/bridge/obs/drive，各 60 秒，
# 四个臂都没有截止期错失、没有整帧缺失，五轴 30° 往返全部走到位）。但那一轮**监控
# 半边一帧都没收到**——原因是主站起的时候没开观测开关，而脚本当时既不设它、也不判
# "到底收到帧没有"（见上面「观测开关」那段）。这一轮之后才补上开关与判定。
# 也就是说：**控制侧已被真机验证过，监控侧的判定逻辑还没有在真机上跑过。**
set -u

REPO=${REPO:-/home/orangepi/ethercat-master}
DEPLOY=${DEPLOY:-orangepi-bench-quint-30deg}
BUILD=${BUILD:-$REPO/build}
MASTER_BIN=${MASTER_BIN:-$BUILD/tools/master/emaster-master}
DUR=${DUR:-60}                 # 每个 GUI 臂跑多久
ROUNDS=${ROUNDS:-1}
# drive 臂由界面端发起的动作：经典的 30° 往返，每趟想要 3 秒（实际由点动速度定，
# 默认 10°/s ⇒ 正好 3 秒，与经典台架测试一个形状）。
RECIP_DEG=${RECIP_DEG:-30}
RECIP_TRAVERSE=${RECIP_TRAVERSE:-3}
ARMS=${ARMS:-solo bridge obs}
CMD_PORT=${CMD_PORT:-5001}
OBS_PORT=${OBS_PORT:-5002}
EVIDENCE_DIR=${EVIDENCE_DIR:-$REPO/reports/_gui}
EVIDENCE=$EVIDENCE_DIR/evidence_e2e.txt
RUN_LOG=/tmp/gui_e2e_run.txt
# 逐臂存档报告的地方。可覆盖**是为了让判据那一段能被离线检查**（scripts/checks/
# bench_gui_e2e_summary_check.sh 就把它指到自己的夹具目录）。夹具要是写进真目录，
# 检查跑一次就会在台架证据目录里留下六份假报告。
RPT_DIR=${RPT_DIR:-/tmp}

SOCK=/tmp/emaster-${DEPLOY}.sock
# 观测口。**它存不存在取决于主站的 EMASTER_OBSERVATION**，不是这张脚本说了算——
# 所以下面每个需要观测的臂都要真的去看一眼它在不在。
OBS_SOCK=/tmp/emaster-${DEPLOY}-obs.sock

# 界面的自检日志判定器。**看它的退出码，不是看它打印了什么。**
GUI_EVIDENCE=${GUI_EVIDENCE:-$REPO/tools/gui_evidence.py}
# drive 臂"走到过多少度"的门槛：按幅度的 2/3 算。摆幅是可配的（RECIP_DEG），
# 门槛要是写死 20°，把幅度调到 10° 就会误判成"轴没动"。
MIN_PEAK_DEG=$(awk -v d="$RECIP_DEG" 'BEGIN{printf "%.1f", d * 2 / 3}')

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 root（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）" >&2
    exit 1
fi

# 非交互 bash 不读 .bashrc，ulimit -r 默认 0，主站拿不到 SCHED_FIFO 会**拒绝进入 OP**。
# sudo 救不了：这是 rlimit 不是权限。
ulimit -r unlimited 2>/dev/null
if [ "$(ulimit -r)" != "unlimited" ]; then
    echo "错误：ulimit -r 仍是 $(ulimit -r)，主站会拒绝进入 OP，整轮白跑" >&2
    exit 1
fi

if [ ! -x "$MASTER_BIN" ]; then
    echo "错误：找不到主站可执行文件 $MASTER_BIN" >&2
    exit 1
fi

# drive 臂会让轴真动。**"确认过"要留下痕迹**，不能只是脚本头上写了句注释让人自己看：
# 得在命令行上多写一个 CONFIRM_MOTION=yes，才算说过"我知道这一跑要动"。
#
# 拦在这里而不是拦在 arm() 里，是因为拦晚了没意义——那时候前面几臂已经把台架跑过了，
# 而现场状况（5 号从站烫不烫、轴周围有没有人）是**开跑之前**看完的。
case " $ARMS " in
    *" drive "*)
        if [ "${CONFIRM_MOTION:-no}" != "yes" ]; then
            cat >&2 <<'MOTION'
错误：ARMS 里有 drive，这一跑会让轴真动，但没有显式确认。
  · 先看清现场：轴周围没人、没夹具，5 号从站（小电机）不烫。
  · 确认后这样跑：
      sudo ARMS='solo obs drive' CONFIRM_MOTION=yes bash scripts/bench_gui_e2e.sh
  · 只要 A/B 证据、不想动轴，把 drive 去掉（默认就是这样）。
MOTION
            exit 1
        fi
        echo "** 已确认要动轴（CONFIRM_MOTION=yes）：drive 臂会走高经典往返" \
             "（全轴同时 ${RECIP_DEG}°、每趟约 ${RECIP_TRAVERSE} 秒）" >&2
        ;;
esac

# ---------------------------------------------------------------------------
# /tmp 的老坑（从 bench_ab_observation.sh 抄过来的，代价是丢过一整臂的数据）
#
# /tmp 是 1777 sticky，且 fs.protected_regular=2（Ubuntu 默认）**连 root 也拦**：
# 已存在的、属主不是自己的文件，root 打不开来写。只要有过一次以别的用户跑起来，
# 之后每次 root 跑都会静默失败。所以自己要先写的临时文件一律先删后建。
mkdir -p "$EVIDENCE_DIR"
rm -f "$RUN_LOG" /tmp/gui_e2e.pid /tmp/gui_e2e.obs /tmp/gui_e2e_master.log \
      /tmp/gui_e2e_master_*.log /tmp/gui_e2e_bridge.log /tmp/gui_e2e_gui_*.txt \
      /tmp/gui_e2e_verdict.txt 2>/dev/null
: > "$RUN_LOG"

# 逐臂存档的那批报告：**开跑前先清掉**。收尾的汇总按 $RPT_DIR/gui_e2e_report_*_r<轮>_<臂>.json
# 找文件，上一轮留下的同名文件会被当成"这一轮的臂"，于是表里凭空多出几行（2026-09-18
# 那次就多出一行 obs，读表的人会以为那一臂跑了两遍）。清的是本脚本自己写的文件，
# 文件名前缀是它的，不是别人的。
rm -f "$RPT_DIR"/gui_e2e_report_*.json 2>/dev/null

say() { echo "$@" | tee -a "$RUN_LOG"; }

# 给"stdout 被调用方 $(...) 接走"的那几个函数用。那些函数里**不能调 say**：
# say 往 stdout 写，会跟函数真正要返回的值粘在一起（父脚本 bench_ab_observation.sh
# 就为这条栽过——状态值多一个字，`[ "$st" = "4" ]` 就永远不成立，表现为"主站一直没
# 进 OP"）。所以错误信息走 stderr，日志照记。
say_err() { echo "$@" >&2; echo "$@" >> "$RUN_LOG"; }

# ---------------------------------------------------------------------------
# 失败时的说明一律走 stderr：stdout 只留那个路径。诊断信息要是打在 stdout 上，
# 就会被 $(...) 收进 REPORT_REL，于是 `[ -z ... ]` 判它非空、当成读成功了，
# 最终 REPORT 会是一个带错误文字的路径——比直接空着更难查。
REPORT_REL=$(DEPLOYMENT="$DEPLOY" DEPLOYMENTS_DIR="$REPO/config/deployments" python3 -c "
import json, os, glob, sys
target = os.environ['DEPLOYMENT']
where = os.path.join(os.environ['DEPLOYMENTS_DIR'], '*.json')
for path in sorted(glob.glob(where)):
    try:
        with open(path, encoding='utf-8') as handle:
            data = json.load(handle)      # 只读一次：开两遍既漏句柄，报错也会晚一步
    except (OSError, ValueError):
        continue
    if data.get('deployment_id') == target:
        if 'run_report_path' not in data:
            print('** 部署 %s 的配置里没有 run_report_path 字段' % target,
                  file=sys.stderr)
            sys.exit(1)
        print(data['run_report_path'])
        sys.exit(0)
print('** 部署目录里没有 deployment_id=%s 的配置' % target, file=sys.stderr)
sys.exit(1)
")
if [ -z "$REPORT_REL" ]; then
    echo "错误：读不出部署 $DEPLOY 的 run_report_path" >&2
    exit 1
fi
# 配置里给的是相对路径（全仓的 run_report_path 都是相对仓库根的），但别把这条当契约：
# 真给了绝对路径，拼出来会是一个不存在的怪路径，而报告那时看起来只是"没写出来"。
case "$REPORT_REL" in
    /*) REPORT=$REPORT_REL ;;
    *)  REPORT=$REPO/$REPORT_REL ;;
esac

# 字段清单**只有这一处**。判据循环和逐臂打印都读它，加字段时不会只加了一边。
#
# 名字全部对着 src/audit/report_sections.c 核过（字段在报告里是**嵌套**的，
# 比如 frame_interval_max_ns 在 process_data_delivery 段里；这里按名字抓，
# 因为 JSON 落盘是每行一个字段，行内 grep 抓得到）。
#
# **参与判定的**只有这两个计数。
#
# frame_interval_max_ns 故意**不**参与判定，只打印：它是最大值，跟两个计数不一样，
# 差个几十微秒是台架逐轮的正常抖动，没有判别力。真拿 `>` 去比它，每轮都会报"劣于
# 基线"，而那种报警读两次之后就没人看了——比不报警更坏。台架上真正有判别力的正是
# 计数：截止期错失的**次数**、整帧缺失的**次数**（"四轴600s往返实测"记的就是
# 4 次 vs 26 次这种整帧缺失）。看极值变化请直接看那一列的数，别让脚本替你下结论。
COMPARE_FIELDS="deadline_missed_count frame_interval_gap_count"
# 打印 + 查齐全性的字段（判定的两个 + 极值 + 现场量）：
PRINTED_FIELDS="$COMPARE_FIELDS frame_interval_max_ns"
CONTEXT_FIELDS="cycle_count wkc_mismatch_count wkc_no_frame_count frame_timeout_us tail_max_receive_ns over_budget_cycle_count sync0_late_count"

metric_grep() {
    local pat; pat=$(echo $PRINTED_FIELDS $CONTEXT_FIELDS | tr ' ' '|')
    grep -oE "\"($pat)\": *[0-9]+" "$REPORT" | sort -u
}

# 取一个字段的数值，取不到就是空串。
#
# **空串必须是"没证据"，不能当成 0。** 判据循环那边专门为这件事分了支：字段读不到
# 时不许静默跳过——跳过的长相正是"这一项不劣于基线"，一条永远为真的判据比没有判据
# 更坏。2026-09-18：写这个脚本时用一份旧报告试过，那份报告里好几个字段都没有，
# 而当时的代码会把它判成一轮漂亮的通过。
metric() {   # $1=报告文件 $2=字段名
    grep -oE "\"$2\": *[0-9]+" "$1" 2>/dev/null | head -1 | grep -oE '[0-9]+$'
}

# 一份报告里，判据字段缺了哪几个。缺了就打印出来（空的表示齐了）。
missing_fields() {   # $1=报告文件
    local k missing=""
    for k in $PRINTED_FIELDS; do
        [ -z "$(metric "$1" "$k")" ] && missing="$missing $k"
    done
    printf '%s' "$missing"
}

clean() {
    pkill -9 -f emaster-master 2>/dev/null
    pkill -9 -f socket_bridge.py 2>/dev/null
    for _ in $(seq 1 10); do pgrep -f 'tools/master/emaster-master' >/dev/null || break; sleep 1; done
    rm -f /tmp/emaster-*.sock
}

start_master() {   # $1 = 要不要开观测（0/1）
    local want_obs=${1:-0}
    # 开关只对**起主站的这一条命令**有效，所以塞进 setsid 的那层 bash 里，不要 export
    # 到本脚本：solo 臂要的是"和加装观测之前逐字一致"的基线，多一个环境变量就不是了。
    local env_prefix=""
    # **要经 `env`，不能写成 `exec VAR=值 主站`**：exec 后面不接受"赋值前缀"这种写法，
    # 它会把 EMASTER_OBSERVATION=1 当成要执行的程序名，于是主站根本没起来，报的是
    # "exec: EMASTER_OBSERVATION=1: 未找到"。2026-09-18 用假主站干跑时抓到的，
    # 是这一轮新写进去的错。`VAR=值 命令` 那种写法只对普通的简单命令成立。
    [ "$want_obs" = "1" ] && env_prefix="env EMASTER_OBSERVATION=1 "
    rm -f "$OBS_SOCK"
    setsid bash -c "echo \$\$ > /tmp/gui_e2e.pid; cd $REPO; exec ${env_prefix}$MASTER_BIN --deployment $DEPLOY" \
        > /tmp/gui_e2e_master.log 2>&1 &
    # pid 文件必须真的写进去：写不进去时后面的 kill -INT 会打空，而打空的长相是
    # "主站已自行退出"——一个看起来完全正常的假象，代价是整臂的停机路径和报告。
    for _ in $(seq 1 20); do [ -s /tmp/gui_e2e.pid ] && break; sleep 0.1; done
    if ! grep -qE '^[0-9]+$' /tmp/gui_e2e.pid 2>/dev/null; then
        say_err "** 错误：/tmp/gui_e2e.pid 没写进去，停机将失去目标"
        return 1
    fi
    for _ in $(seq 1 60); do [ -S "$SOCK" ] && break; sleep 0.5; done
    local st=0
    for _ in $(seq 1 60); do
        st=$(timeout 3 nc -U "$SOCK" <<< "status" 2>/dev/null \
                | grep -o 'state=[0-9]*' | head -1 | cut -d= -f2)
        [ "$st" = "4" ] && break
        sleep 1
    done
    # 观测套接字有没有建起来。**走文件，不走 stdout**：这个函数是用 $(...) 接的、
    # 跑在子壳里，在里面设的变量传不出来；而往 stdout 上再挤一个字，上面那个状态值
    # 就比较不成了（父脚本为这条栽过一次，注释就在 say_err 那儿）。
    local obs_up=0
    if [ "$want_obs" = "1" ]; then
        for _ in $(seq 1 40); do [ -S "$OBS_SOCK" ] && { obs_up=1; break; }; sleep 0.25; done
    fi
    printf '%s' "$obs_up" > /tmp/gui_e2e.obs
    # 只往 stdout 写状态值：调用方用 $(...) 接它，多打一个字就会让比较失败
    printf '%s' "$st"
}

stop_master() {
    local MPID; MPID=$(cat /tmp/gui_e2e.pid 2>/dev/null)
    # 不认文件，按进程名兜底重解析一次：pid 指向别的进程时 `kill -0` 立刻失败，
    # 会被读成"主站已自行退出"，而主站其实还在跑，最后死于下一臂的 pkill -9——
    # 停机路径和报告一起丢，驱动器停在使能态。
    if ! tr '\0' ' ' < "/proc/$MPID/cmdline" 2>/dev/null | grep -q emaster-master; then
        local REAL; REAL=$(pgrep -f 'tools/master/emaster-master' | head -1)
        say "** gui_e2e.pid=$MPID 不是主站或已消失，按进程名重解析为 ${REAL:-无}"
        MPID=${REAL:-$MPID}
    fi
    # 根本没有主站可停（启动就失败了，或者它自己先退了）。这里要**明确收场**：
    # 往下走的话 kill -INT "" 打空、然后老老实实等满 60 秒，最后报"没退出、强杀"——
    # 一份看不出所以然的等待和一条指错方向的结论。
    if [ -z "$MPID" ]; then
        say "没有在跑的主站可停（启动失败或已自行退出），本臂没有报告"
        return 1
    fi
    kill -INT "$MPID" 2>/dev/null
    for _ in $(seq 1 120); do
        kill -0 "$MPID" 2>/dev/null || { say "主站已有序退出"; return 0; }
        sleep 0.5
    done
    say "** 主站 60 秒内没退出，强杀（这一臂的停机路径没走完）"
    pkill -9 -f emaster-master
    return 1
}

# 报告是不是本臂写出来的。只看 mtime 前进，不看内容——报告没写出来时 grep 照样有
# 输出，只是那些数字全是上一轮的，而且逐字段一模一样才看得出来。
report_is_fresh() {
    [ -f "$REPORT" ] || { say "** 报告不存在：$REPORT"; return 1; }
    local now; now=$(stat -c %Y "$REPORT" 2>/dev/null)
    if [ "$now" = "${RPT_BEFORE:-}" ]; then
        say "** 报告未更新：本臂的停机/报告路径没走到，下面若有数字那是上一轮的"
        return 1
    fi
    return 0
}

start_bridge() {
    setsid bash -c "cd $REPO; exec python3 tools/socket_bridge.py --deployment $DEPLOY \
        --bind 127.0.0.1 --port $CMD_PORT --obs-port $OBS_PORT" \
        > /tmp/gui_e2e_bridge.log 2>&1 &
    for _ in $(seq 1 40); do
        grep -q "监听" /tmp/gui_e2e_bridge.log 2>/dev/null && return 0
        sleep 0.25
    done
    say "** 桥 10 秒内没起来，本臂作废"
    tail -5 /tmp/gui_e2e_bridge.log
    return 1
}

# ---------------------------------------------------------------------------
# 一个臂。
#   $1=标签 $2=起桥(0/1) $3=起界面(no|obs|drive) $4=主站要不要开观测(0/1)
#
# 第 4 个参数**不是**"$3 是不是 obs"：solo 臂故意关着（基线要和加装观测之前逐字
# 一致），而 bridge 臂要开着——它是"桥在、界面不在"那根标尺，得和 obs 臂同一个
# 主站配置，否则两臂之间差的就不只是"界面在不在"了。
arm() {
    local TAG=$1 USE_BRIDGE=$2 GUI_MODE=$3 NEED_OBS=${4:-0}
    local STORE=/tmp/gui_e2e_master_${SEQ}_${TAG}.log
    local GUILOG=/tmp/gui_e2e_gui_${SEQ}_${TAG}.txt
    SEQ=$((SEQ+1))

    local RPT_BEFORE=""
    [ -f "$REPORT" ] && RPT_BEFORE=$(stat -c %Y "$REPORT" 2>/dev/null)

    clean
    say ""
    say "################ 臂 $TAG  桥=$USE_BRIDGE 界面=$GUI_MODE  $(md5sum "$MASTER_BIN" | cut -c1-8) ################"
    cd "$REPO" || return 1

    local ST; ST=$(start_master "$NEED_OBS")
    say "主站 state=$ST（观测开关=$NEED_OBS）"
    # 这一步要在观测那条**之前**：主站压根没起来的时候，说"没建观测套接字"是把人往
    # 错的方向带（真原因是启动就失败了，原因就在下面 tail 的那几行里）。干跑时正是
    # 这么演过一次：起主站的那行命令写错，而日志先喊的是"开关没生效"。
    if [ "$NEED_OBS" = "1" ] && [ "$ST" = "4" ]; then
        if [ "$(cat /tmp/gui_e2e.obs 2>/dev/null)" = "1" ]; then
            say "观测套接字已在：$OBS_SOCK"
        else
            # 不在这里 return：控制侧（A/B 的两个计数、掉出 OP）照样是有效的证据，
            # 丢掉它反而更亏。监控半边会在收尾的判定那一步判红。
            say "** 主站没建观测套接字 $OBS_SOCK —— EMASTER_OBSERVATION 没生效"
            say "** 这一臂的监控半边会是空的；往下照跑，但收尾判定会判红"
        fi
    fi
    if [ "$ST" != "4" ]; then
        tail -5 /tmp/gui_e2e_master.log | tee -a "$RUN_LOG"
        # **也要走停机**，不能直接 return：没进 OP 不等于进程没在跑。留着它，
        # 下一臂开头的 clean 会 pkill -9 ——那正是要避开的路径（有序停机换来的报告
        # 和"驱动器不被留在使能态"都没了）。
        stop_master
        return 1
    fi

    if [ "$USE_BRIDGE" = "1" ]; then
        if ! start_bridge; then
            say "桥没起来，本臂作废"
            stop_master      # 同样不能裸 return：主站还开着
            return 1
        fi
        say "桥已起（回环 $CMD_PORT/$OBS_PORT）"
    fi

    if [ "$GUI_MODE" != "no" ]; then
        local EXTRA=""
        # 只读臂**不给命令端点**：界面于是连不上命令口，也就不可能发目标。
        # 这不是省事，是让"只读"这件事由**没有端点可连**来保证，而不是由界面自律。
        local CONN=""
        [ "$GUI_MODE" = "obs" ] && CONN="--obs-endpoint tcp:127.0.0.1:$OBS_PORT"
        # --tcp 一次给两个口（命令 PORT、观测 PORT+1），比分别写两个端点少一处写错的机会
        # drive 臂走的是**经典的 30° 往返**：起点 → +30° → 起点，只在正方向这一侧，
        # 与 tools/test_external_motion_client.py --traverse 3 同形（10°/s ⇒ 3 秒一趟）。
        # 界面自己靠点动速度算一趟多久，所以这里给的是"想要几秒"，实际不会快过
        # 30 ÷ 10 = 3 秒。
        [ "$GUI_MODE" = "drive" ] && CONN="--tcp 127.0.0.1:$CMD_PORT" \
            && EXTRA="--selftest-recip $RECIP_DEG --selftest-traverse $RECIP_TRAVERSE"
        # 界面自己也绑核（rt_affinity 从 /proc 读主站的实时核，躲开它们）。
        # PYTHONPATH 要给到 tools/：界面是包（emaster_gui），不从仓库根 import 得到。
        QT_QPA_PLATFORM=offscreen PYTHONPATH="$REPO/tools" timeout $((DUR+60)) \
            python3 -m emaster_gui $CONN \
            --selftest "$DUR" --no-confirm $EXTRA \
            > "$GUILOG" 2>&1
        local RC=$?
        say "界面退出码=$RC（日志 $GUILOG）"
        if [ "$RC" != "0" ]; then
            say "** 界面没有干净退出，本臂的界面侧证据不可用"
            tail -20 "$GUILOG" | tee -a "$RUN_LOG"
        fi
        say "--- 界面自检报告 ---"
        sed -n '/---- GUI 自检 ----/,/---- 自检结束 ----/p' "$GUILOG" | tee -a "$RUN_LOG"

        say "--- 这一臂的监控/控制判定 ---"
        # **看退出码**。上一轮台架就是只把这几行打印出来，于是"观测：一帧都没收到"
        # 躺在输出里、纸面上却算通过：打印事实的地方不负责说这算不算数。
        if [ ! -f "$GUI_EVIDENCE" ]; then
            say "** 判定器不在：$GUI_EVIDENCE —— 这一臂的监控半边没判，不算通过"
            GUI_VERDICT=2
        else
            python3 "$GUI_EVIDENCE" --expect "$GUI_MODE" \
                --min-peak-deg "$MIN_PEAK_DEG" "$GUILOG" \
                > /tmp/gui_e2e_verdict.txt 2>&1
            GUI_VERDICT=$?
            tee -a "$RUN_LOG" < /tmp/gui_e2e_verdict.txt
        fi
        case "$GUI_VERDICT" in
            0) say "界面侧判定：通过（$GUI_MODE 臂的判据全部成立）" ;;
            1) say "** 界面侧判定：有判据不成立 —— 这一臂的监控/控制**不算通过**" ;;
            *) say "** 界面侧判定：这份日志判不了 —— 不是「没通过」，是「没证到」" ;;
        esac
    else
        sleep "$DUR"
    fi

    stop_master
    local STOPPED=$?

    say "--- 报告指标 ---"
    if report_is_fresh; then
        metric_grep | tee -a "$RUN_LOG"
        cp "$REPORT" "$RPT_DIR/gui_e2e_report_${SEQ}_${TAG}.json" 2>/dev/null || true
    else
        say "（本臂报告不可用，见上；不要用任何数字）"
    fi
    say "--- 停机 AL 快照 ---"
    grep -E 'AL state=' /tmp/gui_e2e_master.log | tail -8 | tee -a "$RUN_LOG"
    grep -E '总线状态' /tmp/gui_e2e_master.log | tail -1 | cut -c1-160 | tee -a "$RUN_LOG"
    cp /tmp/gui_e2e_master.log "$STORE" 2>/dev/null || true
    cp /tmp/gui_e2e_bridge.log "/tmp/gui_e2e_bridge_${SEQ}_${TAG}.log" 2>/dev/null || true
    [ "$STOPPED" = "0" ] || say "** 这一臂的停机路径没走完，数据只作参考"
}

# ---------------------------------------------------------------------------
say "==== 界面端到端 + 不干扰 A/B  ===="
say "部署=$DEPLOY  时长=${DUR}s  轮数=$ROUNDS  臂='$ARMS'"
say "主站=$MASTER_BIN（$(md5sum "$MASTER_BIN" | cut -c1-8)）"
say "证据写到 $EVIDENCE"

SEQ=0
for ROUND in $(seq 1 "$ROUNDS"); do
    say ""
    say "======== 第 $ROUND / $ROUNDS 轮 ========"
    for A in $ARMS; do
        case "$A" in
            # solo 故意关着观测：基线要能和加装观测之前逐字对照。代价写在收尾的
            # 「没证到的」里——"不劣于基线"因此含着观测发布本身的代价。
            solo)   arm "r${ROUND}_solo"   0 no    0 ;;
            bridge) arm "r${ROUND}_bridge" 1 no    1 ;;
            obs)    arm "r${ROUND}_obs"    1 obs   1 ;;
            drive)  arm "r${ROUND}_drive"  1 drive 1 ;;
            *)      say "未知臂 $A（跳过）" ;;
        esac
    done
done

clean

# ---------------------------------------------------------------------------
# 汇总：每个非 solo 臂跟本轮 solo 比。**写成"不劣于基线"**，不是"零错"。
say ""
say "################ 汇总（A/B）################"
{
    echo "==== 界面端到端 + 不干扰 A/B 汇总 ===="
    echo "部署=$DEPLOY 时长=${DUR}s 轮数=$ROUNDS 臂='$ARMS'"
    echo "主站 md5=$(md5sum "$MASTER_BIN" | cut -c1-8)  时间=$(date -Is)"
    echo ""
    echo "对比项（越小越好）：deadline_missed_count / frame_interval_gap_count / frame_interval_max_ns"
    echo "判据：界面臂**不劣于同轮 solo 基线**。基线自己有多少错是台架当时的状况。"
    echo ""

    # 这一段在函数外，**不能用 local**（bash 的 local 只在函数里有效，在顶层用会直接
    # 报 "local: can only be used in a function"）。变量就取了带前缀的名字避免串味。
    for ROUND in $(seq 1 "$ROUNDS"); do
        local_base=$(ls $RPT_DIR/gui_e2e_report_*_r${ROUND}_solo.json 2>/dev/null | head -1)
        if [ -z "$local_base" ]; then
            echo "第 $ROUND 轮：没有 solo 基线报告，这一轮比不了"
            continue
        fi
        echo "--- 第 $ROUND 轮（基线 $(basename "$local_base")）---"
        printf '%-16s %10s %10s %12s\n' "臂" "截止期错失" "整帧缺失" "帧距最大ns"
        printf '%-16s %10s %10s %12s\n' "solo(基线)" \
            "$(metric "$local_base" deadline_missed_count | sed 's/^$/-/')" \
            "$(metric "$local_base" frame_interval_gap_count | sed 's/^$/-/')" \
            "$(metric "$local_base" frame_interval_max_ns | sed 's/^$/-/')"
        VERDICT=0
        # 比过的项里，基线本来是 0 的有几个。这个数决定结论那句话该不该带"没有分辨率"
        # 那句注脚——注脚只在**真的**没分辨率时才成立。第一版不看这个，基线明明是 1
        # 也照样附上注脚，等于把一条准确的话说歪了。
        ZEROBASE=0
        COMPARED=0
        UNPROVEN=0
        # 基线自己缺字段的话，整轮都判不了——先把话说在前面。
        BASEMISS=$(missing_fields "$local_base")
        [ -n "$BASEMISS" ] && echo "** 基线报告缺字段：$BASEMISS —— 这些项本轮比不了"
        for f in $RPT_DIR/gui_e2e_report_*_r${ROUND}_*.json; do
            case "$f" in *"_r${ROUND}_solo.json") continue ;; esac
            [ -f "$f" ] || continue
            ARMNAME=$(basename "$f" | sed -E 's/.*_r[0-9]+_(.*)\.json/\1/')
            # 变量名别叫 fi：fi 是 bash 的保留字，赋值直接是语法错误。
            DMISS=$(metric "$f" deadline_missed_count)
            GAPS=$(metric "$f" frame_interval_gap_count)
            FMAX=$(metric "$f" frame_interval_max_ns)
            printf '%-16s %10s %10s %12s\n' "$ARMNAME" "${DMISS:--}" "${GAPS:--}" "${FMAX:--}"
            ARMMISS=$(missing_fields "$f")
            if [ -n "$ARMMISS" ]; then
                echo "** $ARMNAME 缺字段：$ARMMISS —— 这些项没比，不算通过"
                UNPROVEN=$((UNPROVEN+1))
            fi
            # 基线是 0 的时候不判"更差"——0 和 0 之间没有分辨率，硬判只会噪声报警。
            # 字段名从 $COMPARE_FIELDS 来，别在这儿再抄一遍：抄一遍就会有一天只改了一边。
            for KEY in $COMPARE_FIELDS; do
                VAL=$(metric "$f" "$KEY")
                BASEV=$(metric "$local_base" "$KEY")
                if [ -z "$VAL" ] || [ -z "$BASEV" ]; then
                    continue          # 缺证据，已经在上面点名了，这里不重复计数
                fi
                COMPARED=$((COMPARED+1))
                [ "$BASEV" = "0" ] && ZEROBASE=$((ZEROBASE+1))
                if [ "$VAL" -gt "$BASEV" ]; then
                    echo "** $ARMNAME 的 $KEY = $VAL，比基线的 $BASEV 差"
                    VERDICT=1
                fi
            done
        done
        if [ "$VERDICT" != "0" ]; then
            echo "结论：** 有不劣于基线这条不成立的项，见上"
        elif [ "$UNPROVEN" != "0" ]; then
            echo "结论：** 有臂的判据字段没读全，这一轮只能算「没证到」，不算通过"
        elif [ -n "$BASEMISS" ]; then
            # 和"臂没报告"分开说：这里的报告是有的，只是基线自己不可用，
            # 两者要查的地方完全不同（一个去查报告有没有落盘，一个去查写入器）。
            echo "结论：** 基线报告本身缺字段，这一轮无从比较（不是臂的问题）"
        elif [ "$COMPARED" = "0" ]; then
            echo "结论：本轮的臂没有报告可比，什么都不算"
        elif [ "$ZEROBASE" = "$COMPARED" ]; then
            echo "结论：都不劣于基线——但比过的 $COMPARED 项基线全是 0，"
            echo "      0 和 0 之间没有分辨率，这只说明「没看出差别」，不是「没有代价」。"
        else
            echo "结论：都不劣于基线（其中 $ZEROBASE/$COMPARED 项基线本来就是 0，那几项没有分辨率）"
        fi
        echo ""
    done

    echo "--- 没证到的 ---"
    echo "1. **基线为 0 时 A/B 没有分辨率。** deadline_missed_count 如果 solo 就是 0，"
    echo "   界面臂也是 0，这只说明「没看出差别」，不说明「没有代价」。要看出代价得跑"
    echo "   更久（ROUNDS/DUR 调大），或者去找一个基线本来就有错的负载条件。"
    echo "2. **曲线的形状对不对**：本脚本只证界面连上了、收到了帧、自己没掉帧。"
    echo "   像素画得对不对只能靠人眼看（曲线模块的检查里也是这么写的）。"
    echo "3. **跨机器那条路（Windows 界面 + 局域网桥）本脚本不走**：自动取证走回环，"
    echo "   跨机器要显式 --bind 管理口 IP，是手动验证那条路。"
    echo "4. **界面被杀死的处置**：本脚本用 timeout 兜底，没验过主站在界面卡死时"
    echo "   是什么表现（命令口的 5 秒空闲超时应该会收掉连接，但没实测）。"
    echo "5. **solo 基线的观测是关着的**（要能和加装观测之前逐字对照）。所以"
    echo "   「不劣于基线」这句话里头含着观测发布本身的代价，没有把它单独摘出来量。"
    echo "   要摘出来得再加一个「solo + 观测」的臂——本脚本没有这个臂。"
    echo "6. **判定器认的是界面报告的措辞**（tools/gui_evidence.py）。界面改了那几行字，"
    echo "   它就判不了——那时会走「判不了」（退出码 2）而不是安静判过，但两边要一起改。"
} | tee -a "$RUN_LOG"

mkdir -p "$EVIDENCE_DIR"
cp "$RUN_LOG" "$EVIDENCE"
say ""
say "已完成。证据：$EVIDENCE"
