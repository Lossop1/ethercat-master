#!/bin/bash
# 台架取证脚本的判据那一段，离线验。
#
#   bash scripts/checks/bench_gui_e2e_summary_check.sh
#
# 为什么值得单独验：`scripts/bench_gui_e2e.sh` 的 A/B 结论是**这一层抢不抢资源**的
# 唯一判据。它是一段 shell + grep + 字符串比较，写歪了不会崩、不会报错，只会安静地
# 给出一个结论——而且往往是最想看到的那个结论（"不劣于基线"）。台架上一轮要跑几分钟、
# 还要占着硬件，判据对不对不能等到那时候才知道。
#
# 验的是**真脚本里那一段**，不是抄一份：字段定义、metric()、missing_fields() 和整个
# 汇总块都是从 scripts/bench_gui_e2e.sh 里抠出来 eval 的。抄一份的话，真脚本改了这边
# 不会跟着改，检查就成了摆设。
#
# 夹具写在自己的目录里（RPT_DIR），不碰 /tmp 下真跑出来的报告。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# 第一个参数可以指定别的脚本文件。**这是为变异测试留的**：要验"这个检查真的会红"，
# 就得把被检查的那段改坏。改真脚本再改回来这种事，2026-09-18 干过一次——工具调用在
# "改完了、还没还原"的当口被掐断，脚本就带着一个"永远判通过"的变异留在工作区里，
# 而它长得跟正常代码一模一样。改成指向副本之后，真脚本就没这个机会了。
SCRIPT=${1:-$ROOT/scripts/bench_gui_e2e.sh}
FIX=/tmp/gui_e2e_ab_fixtures

if [ ! -f "$SCRIPT" ]; then echo "找不到 $SCRIPT" >&2; exit 2; fi
if ! command -v bash >/dev/null; then echo "需要 bash" >&2; exit 2; fi

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf 'ok    %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf 'FAIL  %s\n      期望包含：%s\n      实际：%s\n' "$1" "$2" "$3"; }

# ── 抠真脚本里的定义与汇总块 ────────────────────────────────────────────────
DEFS=$(awk '
    /^COMPARE_FIELDS=/    { on=1 }
    on                    { print }
    /^missing_fields\(\)/ { seen=1 }
    seen && /^}$/         { exit }
' "$SCRIPT")
BLOCK=$(awk '/^say "####.*汇总（A\/B）/{grab=1; next} grab && /^} \| tee -a "\$RUN_LOG"/{print; exit} grab' "$SCRIPT")
if [ -z "$DEFS" ] || [ -z "$BLOCK" ]; then
    echo "抠不出字段定义或汇总块——scripts/bench_gui_e2e.sh 的结构变了，" >&2
    echo "这个检查得跟着改（它是照着那两个锚点找的）。" >&2
    exit 2
fi

# ── 夹具与跑法 ──────────────────────────────────────────────────────────────
# 一份"齐全"的报告：三个打印字段都在。缺字段的场景另外造。
report() {   # $1=序号_臂   $2=截止期错失 $3=整帧缺失 $4=帧距最大ns
    printf '{"cycle_count": 60000, "deadline_missed_count": %s,
             "frame_interval_gap_count": %s, "frame_interval_max_ns": %s}\n' \
        "$2" "$3" "$4" > "$FIX/gui_e2e_report_$1.json"
}
wipe() { rm -f "$FIX"/gui_e2e_report_*.json; }
setup() { wipe; mkdir -p "$FIX"; }

# 跑一轮（ROUNDS=1，臂 solo + 两个实验臂），把结论行打出来
verdict() {
    # BLOCK 会引用主脚本里的一堆变量（部署名、时长、臂表……）。**都得给**：主脚本自己
    # 是 set -u，汇总块一读到没定义的变量就整个中止，而中止的长相是"结论一行都没有"。
    # 本检查第一版就漏了 DEPLOY，九个场景全报空——看上去像判据写错了，其实是夹具没喂全。
    (
        set -u
        DEPLOY=fixture
        RPT_DIR=$FIX
        DUR=60; ROUNDS=1; ARMS='solo bridge obs'
        MASTER_BIN=/bin/true; EVIDENCE_DIR=$FIX/evidence; RUN_LOG=$FIX/run.txt
        mkdir -p "$EVIDENCE_DIR"; : > "$RUN_LOG"
        eval "$DEFS"
        eval "$BLOCK"
    # 抓三类行：结论本身、结论的续行（缩进六个空格那句）、以及**逐项点名的那些
    # `** ...` 行**。第三类一开始漏了，于是"点名了哪个字段"那两条怎么都过不了——
    # 判据其实报得好好的，是检查自己没把它捞上来。
    ) 2>&1 | grep -E '^结论：|^      |^\*\*'
}

expect() {   # $1=场景名 $2=期望在结论里出现的片段
    local got; got=$(verdict)
    if printf '%s' "$got" | grep -qF "$2"; then ok "$1"; else bad "$1" "$2" "$got"; fi
}

setup

# 1. 干净通过：计数都是 0，极值只差 100 ns。
report 1_r1_solo   0 0 1000000
report 2_r1_bridge 0 0 1000100
report 3_r1_obs    0 0 1000200
expect "计数全 0 时判通过（极值抖动不参与判定）" "都不劣于基线"

# 2. 真的变差：计数涨了，必须报出来，而且两个字段都要点名。
report 1_r1_solo   1 0 1000000
report 2_r1_bridge 1 0 1000100
report 3_r1_obs    4 2 1099000
expect "计数涨了判差" "有不劣于基线这条不成立的项"
V=$(verdict)
for want in "deadline_missed_count = 4" "frame_interval_gap_count = 2"; do
    if printf '%s' "$V" | grep -qF "$want"; then ok "点名了 $want"; else bad "点名了 $want" "$want" "$V"; fi
done

# 3. 基线本来就脏：比的是**相对**，不是绝对零错。
report 1_r1_solo   3 2 1000000
report 2_r1_bridge 3 2 1000100
report 3_r1_obs    3 2 1000300
expect "基线非 0 但它没变差时仍判通过" "都不劣于基线"

# 4. 判据字段缺一个：**不许**当通过。这是本检查最要紧的一条——静默跳过正是
#    "这一项不劣于基线"的长相，一条永远为真的判据比没有判据更坏。
report 1_r1_solo   0 0 1000000
report 2_r1_bridge 0 0 1000100
printf '{"cycle_count": 60000, "deadline_missed_count": 0, "frame_interval_gap_count": 0}\n' \
    > "$FIX/gui_e2e_report_3_r1_obs.json"
expect "臂缺字段时判「没证到」，不算通过" "没证到"
expect "臂缺字段时点名缺了哪个" "frame_interval_max_ns"

# 5. 基线自己缺字段：和"臂没报告"要分开说——两者要查的地方完全不同。
printf '{"cycle_count": 60000}\n' > "$FIX/gui_e2e_report_1_r1_solo.json"
report 2_r1_bridge 0 0 1000100
report 3_r1_obs    0 0 1000200
expect "基线缺字段时判「无从比较」" "基线报告本身缺字段"

# 6. 一个实验臂的报告都没有：不能因为"没比对"就算通过。
report 1_r1_solo 0 0 1000000
wipe; report 1_r1_solo 0 0 1000000
expect "没有可比的臂时判「什么都不算」" "没有报告可比"

wipe
printf '\n%d/%d 项通过\n' "$PASS" "$((PASS+FAIL))"
[ "$FAIL" = 0 ] || exit 1
exit 0
