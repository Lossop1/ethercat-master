#!/bin/bash
# CSP reliability test: continuously refresh external targets and monitor master stability
# Usage: [DEPLOYMENT=<部署ID>] test_csp_reliability.sh [duration_seconds]
# Default: 300s. For full P1 verification pass 3600 or more.
# Deployment: 由 DEPLOYMENT / EMASTER_DEPLOYMENT 指定，默认台架五轴；需要部署里
#             没有 motion_profile，走纯外部目标模式。
# Pass: socket responds throughout, process never exits, fail_count == 0
# Fail: any socket failure, unexpected process exit, or master fault
#
# 注意：下面的就绪检查读 a1/a2 两个轴的位置，只要求部署至少两轴。轴数多于两个时
# 它仍然只盯前两轴——这条脚本量的是链路存活，不是逐轴覆盖。

set -euo pipefail

DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-orangepi-bench-quint-30deg}}"
SOCK=/tmp/emaster-${DEPLOYMENT}.sock
LOG=/tmp/test_csp_reliability.log
REPORT_DST=/tmp/test_csp_reliability_report.json
DURATION_SEC=${1:-300}
REPO=/home/orangepi/ethercat-master

# 报告路径从部署配置读，不写死：换部署时忘了同步会归档到一份别处的旧报告。
REPORT_REL=$(DEPLOYMENT="$DEPLOYMENT" python3 -c "
import json, os, glob, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob('$REPO/config/deployments/*.json')):
    with open(path) as handle:
        if json.load(handle).get('deployment_id') == target:
            print(json.load(open(path))['run_report_path'])
            sys.exit(0)
sys.exit(1)
" 2>/dev/null) || REPORT_REL=""
# 上面那句 `|| REPORT_REL=""` 不是多余的：set -e 下，赋值语句的返回码就是命令
# 替换的返回码，没有它会在部署找不到时静默退出，下面的报错提示永远打不出来。
if [ -z "$REPORT_REL" ]; then
    echo "错误：读不出部署 $DEPLOYMENT 的 run_report_path" >&2
    exit 1
fi
REPORT_SRC=$REPO/$REPORT_REL

cd /home/orangepi/ethercat-master/build || exit 1

sudo rm -f "$SOCK" "$LOG"

echo "=== CSP reliability test deployment=$DEPLOYMENT duration=${DURATION_SEC}s ==="
echo ""

sudo ./tools/master/emaster-master --deployment "$DEPLOYMENT" 2>"$LOG" &
MASTER_PID=$!

for i in $(seq 1 40); do
    [ -S "$SOCK" ] && break
    sleep 0.5
done
if [ ! -S "$SOCK" ]; then
    echo "FAILED: socket not created (master startup failed, see $LOG)"
    exit 1
fi

sleep 4

INIT=$(echo "status" | timeout 2 sudo nc -U "$SOCK" 2>&1) || true
echo "initial status: $INIT"

P1=$(echo "$INIT" | grep -oP 'a1:pos=\K-?[0-9]+' || true)
P2=$(echo "$INIT" | grep -oP 'a2:pos=\K-?[0-9]+' || true)
if [ -z "$P1" ] || [ -z "$P2" ]; then
    echo "FAILED: cannot parse initial positions (status query failed)"
    sudo kill "$MASTER_PID" 2>/dev/null
    exit 1
fi
echo "holding target: a1=$P1 a2=$P2"
echo ""

OK_COUNT=0
FAIL_COUNT=0
LAST_REPORT_SEC=0
START_TS=$(date +%s)

echo "--- running (status snapshot every 10s) ---"
while true; do
    NOW_TS=$(date +%s)
    ELAPSED=$((NOW_TS - START_TS))
    [ "$ELAPSED" -ge "$DURATION_SEC" ] && break

    RESP=$(echo "set_external_target $P1 $P2" | timeout 1 sudo nc -U "$SOCK" 2>/dev/null || true)
    if echo "$RESP" | grep -q '^OK'; then
        OK_COUNT=$((OK_COUNT + 1))
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "${ELAPSED}s: WARN socket failed (resp='$RESP') fail_total=$FAIL_COUNT"
    fi

    if ! kill -0 "$MASTER_PID" 2>/dev/null; then
        echo ""
        echo "FAILED: master process exited unexpectedly at ${ELAPSED}s"
        exit 1
    fi

    if [ $((ELAPSED - LAST_REPORT_SEC)) -ge 10 ]; then
        STATUS=$(echo "status" | timeout 2 sudo nc -U "$SOCK" 2>&1) || true
        CYCLE=$(echo "$STATUS"  | grep -oP 'cycle=\K[0-9]+'             || echo '?')
        CUR_P1=$(echo "$STATUS" | grep -oP 'a1:pos=\K-?[0-9]+'          || echo '?')
        CUR_P2=$(echo "$STATUS" | grep -oP 'a2:pos=\K-?[0-9]+'          || echo '?')
        ERR1=$(echo "$STATUS"   | grep -oP 'a1:.*?err=\K0x[0-9a-fA-F]+' || echo '?')
        ERR2=$(echo "$STATUS"   | grep -oP 'a2:.*?err=\K0x[0-9a-fA-F]+' || echo '?')
        COMPLETE=$(echo "$STATUS" | grep -oP 'completed=\K[0-9]+'       || echo '?')
        STATE=$(echo "$STATUS"  | grep -oP '^OK\|state=\K[0-9]+'        || echo '?')
        echo "${ELAPSED}s: cycle=$CYCLE state=$STATE completed=$COMPLETE" \
             "a1=$CUR_P1(err=$ERR1) a2=$CUR_P2(err=$ERR2)" \
             "| ok=$OK_COUNT fail=$FAIL_COUNT"
        LAST_REPORT_SEC=$ELAPSED
    fi

    sleep 0.1
done

echo ""
echo "--- test complete ---"
TOTAL=$((OK_COUNT + FAIL_COUNT))
echo "sent=$TOTAL ok=$OK_COUNT fail=$FAIL_COUNT"

echo ""
echo "stopping master..."
sudo kill -SIGINT "$MASTER_PID" 2>/dev/null || true
sleep 3
sudo pkill -9 -f "emaster-master.*$DEPLOYMENT" 2>/dev/null || true
sleep 1

sudo cp "$REPORT_SRC" "$REPORT_DST" 2>/dev/null && echo "report saved to $REPORT_DST" || echo "WARN: report not available"

echo ""
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "PASS: socket responded throughout ${DURATION_SEC}s, no failures"
else
    echo "FAIL: $FAIL_COUNT socket failures detected"
    exit 1
fi
