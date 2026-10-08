#!/bin/bash
# 台架多轮测试驱动器：把"跑一轮"的脚本重复 N 轮，并把每一轮的证据归拢到仓库里。
#
#   sudo env DEPLOYMENT=<部署ID> SERIES=<系列名> ROUNDS=6 GAP=60 MODE=reciprocate \
#        bash scripts/bench_series.sh
#
# 为什么要有这一层。排查"这条故障是必现、随机、还是没有"只能靠连跑，而单轮脚本
# （bench_idle.sh / bench_reciprocate_30deg.sh）一次只跑一轮、产物落在 /tmp。连跑
# 十几轮之后，"第几轮是什么形状"就散在一堆 /tmp 文件里，报告又是覆盖式的——上一轮
# 还没看就被下一轮盖掉。这个脚本把那件事收成一件：**每轮一份自足的证据目录**。
#
# 它不自己起主站、不自己发目标，那些仍由单轮脚本负责；这里只管"重复 + 收证据"。
# 所以参数不用在这里再列一遍——环境变量原样被子脚本继承（见下面"参数"）。
#
# 参数（环境变量）：
#   DEPLOYMENT  必填。部署 ID。没有默认值：写一个默认部署等于把某一个台架写进工具里。
#   SERIES      必填。系列名，决定证据目录 reports/_<SERIES>/。
#   ROUNDS      轮数，默认 6。
#   GAP         轮间间隔秒数，默认 60。**这个值本身是变量**：背靠背重启会不会让驱动器
#               来不及就绪，是待查的问题之一，所以间隔要能调、并且记进 index.tsv。
#   MODE        idle | reciprocate，默认 reciprocate，决定调哪个单轮脚本。
#   其余（DURATION／TRAVERSE／DEGREES／RATE／ONLY_AXIS／IDLE／OBSERVATION …）
#               原样传给单轮脚本，各自有各自文档，这里不重复也不校验。
#
# 产物：reports/_<SERIES>/
#         index.tsv            每轮一行（表头来自 report_row.py --header）
#         r<NN>_<TAG>/master.log      主站日志
#         r<NN>_<TAG>/outcome.txt     单轮脚本的全程输出
#         r<NN>_<TAG>/client.log      客户端日志（有的话）
#         r<NN>_<TAG>/summary.txt     report_summary.py 对该轮报告的输出
#         r<NN>_<TAG>/row.tsv         该轮在 index.tsv 里那一行
#         r<NN>_<TAG>/report-name.txt 该轮在归档目录里的完整报告文件名
#         r<NN>_<TAG>/condition.txt     这一轮施加的条件（轴数/时长/节拍/间隔/观测…）
#         r<NN>_<TAG>/device-identity.txt 这一轮跑的是什么代码、什么配置、总线上是哪几台
#         r<NN>_<TAG>/anomalies.tsv     该轮主站日志里的异常，按探索任务书 3.4 节的列
#         r<NN>_<TAG>/notes.md          现场观察记录；"没看懂的"也写在这里
#       **完整报告不入库**（几十到几百 MB），它留在部署配置 run_report_path 所指
#       目录的 archive/ 下，上面那个文件名就是指向它的坐标。
#
# 退出码：0 = 全部轮次都跑完了（**不代表每轮都成功**——成败在 index.tsv 里看）；
#         1 = 环境不对（没有 root、没有 DEPLOYMENT、报告路径解析不出来）。

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

fail() { echo "错误：$1" >&2; exit 1; }

[ "$(id -u)" = "0" ] || fail "必须在 root 下运行（单轮脚本要 mlockall + SCHED_FIFO）"
DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-}}"
[ -n "$DEPLOYMENT" ] || fail "没有给 DEPLOYMENT（也不要用默认值——默认值等于把某个台架写进工具里）"
SERIES="${SERIES:-}"
[ -n "$SERIES" ] || fail "没有给 SERIES（它决定证据目录 reports/_<SERIES>/）"
ROUNDS="${ROUNDS:-6}"
GAP="${GAP:-60}"
MODE="${MODE:-reciprocate}"

case "$MODE" in
    idle)        ROUND_SCRIPT="$REPO/scripts/bench_idle.sh" ;;
    reciprocate) ROUND_SCRIPT="$REPO/scripts/bench_reciprocate_30deg.sh" ;;
    *)           fail "不认识 MODE=$MODE（idle | reciprocate）" ;;
esac
# 查存在与可读，**不查可执行位**：本仓库 scripts/ 下的脚本一律以 `bash <脚本>` 调用，
# 索引里也都是 100644（只有极少数被别的脚本用 [ -x ] 查过的才带可执行位）。
# 这里若查 -x，就需要每个单轮脚本额外带一个权限位，与仓库约定相反。
[ -r "$ROUND_SCRIPT" ] || fail "单轮脚本不存在或不可读：$ROUND_SCRIPT"

# 报告路径按 deployment_id 从配置里找，不按文件名猜（文件名用下划线、ID 用连字符），
# 也不写死——换部署时忘了同步会读完一份别处的旧报告却看不出异常。
# 这一段与 tools/verify_p25_motion.sh 同一条规则。
REPORT_REL=$(DEPLOYMENT="$DEPLOYMENT" python3 -c "
import glob, json, os, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob(os.path.join('$REPO', 'config', 'deployments', '*.json'))):
    with open(path, encoding='utf-8') as handle:
        if json.load(handle).get('deployment_id') == target:
            print(json.load(open(path, encoding='utf-8'))['run_report_path'])
            sys.exit(0)
sys.exit(1)
" 2>/dev/null)
[ -n "$REPORT_REL" ] || fail "读不出部署 $DEPLOYMENT 的 run_report_path"
REPORT="$REPO/$REPORT_REL"
REPORT_DIR="$(dirname "$REPORT")"
ARCHIVE_DIR="$REPORT_DIR/archive"

EVIDENCE="$REPO/reports/_$SERIES"
mkdir -p "$EVIDENCE"
INDEX="$EVIDENCE/index.tsv"
if [ ! -f "$INDEX" ]; then
    # 表头 = 本脚本自己那几列 + report_row.py 的列（两处各写各的会漂，所以合成一次）。
    printf 'series\tdeployment\tmode\tround_at\tgap_s\texit_code\t%s\n' \
        "$(python3 "$REPO/scripts/analysis/report_row.py" --header)" > "$INDEX"
fi

echo "系列=$SERIES 部署=$DEPLOYMENT 模式=$MODE 轮数=$ROUNDS 间隔=${GAP}s"
echo "单轮脚本=$ROUND_SCRIPT"
echo "证据目录=$EVIDENCE"
echo "报告=$REPORT"
echo

for round_index in $(seq 1 "$ROUNDS"); do
    TAG="$(printf '%sr%02d' "$SERIES" "$round_index")"
    ROUND_AT="$(date '+%Y-%m-%d %H:%M:%S')"
    ROUND_DIR="$EVIDENCE/$(printf 'r%02d' "$round_index")_$TAG"
    mkdir -p "$ROUND_DIR"

    # 条件记录。事后比较两轮时，第一步就是确定它们的差别在哪；而轮次是脚本循环出来的，
    # 命令行历史里只有一条命令，看不出每一轮各自的条件。
    {
        echo "series=$SERIES"
        echo "round=$round_index"
        echo "tag=$TAG"
        echo "started_at=$ROUND_AT"
        echo "deployment=$DEPLOYMENT"
        echo "mode=$MODE"
        echo "gap_s=$GAP"
        echo "rounds=$ROUNDS"
        echo "round_script=$ROUND_SCRIPT"
        # 其余条件由单轮脚本继承，这里照样记一份：它们是"运动形态"的全部参数。
        for name in DURATION IDLE TRAVERSE DEGREES RATE WAVE ONLY_AXIS \
                    REPORT_INTERVAL OBSERVATION SETTLE_TICKS; do
            value="${!name:-}"
            [ -n "$value" ] && echo "$(printf '%s' "$name" | tr 'A-Z' 'a-z')=$value"
        done
    } > "$ROUND_DIR/condition.txt"

    # 装置身份：这一轮跑的是什么代码、什么配置、总线上插的是哪几台。
    # 没有它，跨轮次与跨天的比较不成立。采集失败只写一行说明，不让整轮停下 ——
    # 一个派生文件不该拖垮一轮探查。
    python3 "$REPO/scripts/capture_identity.py" "$ROUND_DIR/device-identity.txt" \
        --deployment "$DEPLOYMENT" >/dev/null 2>&1 ||
        echo "capture_error=采集失败（此时 device-identity.txt 不可用，该轮的跨轮比较不成立）" \
            > "$ROUND_DIR/device-identity.txt"

    # 现场记录模板。探查的结论经常来自"没看懂的"那几条，给它一个固定的地方，
    # 免得它们只留在终端滚动里。
    [ -f "$ROUND_DIR/notes.md" ] || cat > "$ROUND_DIR/notes.md" <<'NOTES'
# 观察记录

## 现场

## 没看懂的

NOTES

    echo "===== 第 $round_index/$ROUNDS 轮 $TAG 开始 $ROUND_AT ====="

    # 删掉本轮的旧产物，避免"读到了上一轮的 done"这种假通过。
    rm -f "/tmp/${TAG}_done" "/tmp/${TAG}_master.log" \
          "/tmp/${TAG}_outcome.txt" "/tmp/${TAG}_client.log"

    TAG="$TAG" bash "$ROUND_SCRIPT" >/dev/null 2>&1
    EXIT_CODE=$?
    #
    # 以单轮脚本自己写下的 _done 为准，$? 只作兜底。
    # 理由：单轮脚本的 finish() 一度只 echo 不 exit，于是"跑满／有空档／被主站中断／
    # shutdown 没停住"四种结局在 $? 里全是 0，而 index.tsv 的 exit_code 列取的就是 $?
    # ——收尾掉轴那种轮次会显示成干干净净的一轮（2026-10-08 实测到一次）。
    # finish() 已经补上 exit；这里再以 _done 为准，是因为那个文件是**判定发生的当场**
    # 写下的，比调用方读到的 $? 更接近事实，也能挡住将来再有人漏写 exit。
    # 本轮开始时 _done 已被删掉，所以文件不存在就说明这一轮没走到判定，此时用 $?。
    if [ -f "/tmp/${TAG}_done" ]; then
        EXIT_CODE="$(cat "/tmp/${TAG}_done")"
    fi
    echo "  单轮脚本退出码=$EXIT_CODE（见脚本头部的含义表）"

    for artifact in master.log outcome.txt client.log; do
        [ -f "/tmp/${TAG}_$artifact" ] && cp -f "/tmp/${TAG}_$artifact" "$ROUND_DIR/"
    done
    [ -f "/tmp/${TAG}_done" ] && cp -f "/tmp/${TAG}_done" "$ROUND_DIR/exit-code.txt"

    # 异常表：把主站日志里已经有的坐标固定成一张可跨轮归并的表。只读日志，因此快且
    # 必然可用（报告可能因磁盘或崩溃而缺失）。日志不在就跳过，不让派生文件拖垮一轮。
    if [ -f "/tmp/${TAG}_master.log" ]; then
        python3 "$REPO/scripts/analysis/anomalies.py" "/tmp/${TAG}_master.log" \
            -o "$ROUND_DIR/anomalies.tsv" --quiet 2>>"$ROUND_DIR/notes.md" || true
    fi

    # 完整报告是覆盖式的：先把这一轮对应的归档文件名记下来（那是完整证据的坐标），
    # 再对**当前**那份报告出摘要——两者都小，能入库。
    ARCHIVE_NAME="-"
    if [ -d "$ARCHIVE_DIR" ]; then
        ARCHIVE_NAME="$(ls -t "$ARCHIVE_DIR" 2>/dev/null | head -1)"
    fi
    printf '%s\n' "$ARCHIVE_NAME" > "$ROUND_DIR/report-name.txt"

    ROW="-"
    if [ -f "$REPORT" ]; then
        python3 "$REPO/scripts/analysis/report_summary.py" "$REPORT" \
            > "$ROUND_DIR/summary.txt" 2>&1
        ROW="$(python3 "$REPO/scripts/analysis/report_row.py" "$REPORT" "$TAG" 2>/dev/null)"
        printf '%s\n' "$ROW" > "$ROUND_DIR/row.tsv"
    else
        echo "  这一轮没有报告（主站没起来或没写）" | tee "$ROUND_DIR/summary.txt"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$SERIES" "$DEPLOYMENT" "$MODE" "$ROUND_AT" "$GAP" "$EXIT_CODE" "$ROW" >> "$INDEX"
    echo "  证据=$ROUND_DIR"

    if [ "$round_index" -lt "$ROUNDS" ]; then
        echo "  轮间等待 ${GAP}s（$(date '+%H:%M:%S')）"
        sleep "$GAP"
    fi
    echo
done

echo "全部 $ROUNDS 轮结束。汇总：$INDEX"
echo "**注意**：退出码 0 只表示轮次跑完了，不代表每轮都成功——成败看 index.tsv。"
exit 0
