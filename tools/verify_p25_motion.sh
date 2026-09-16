#!/bin/bash
# P2.5 motion verification: start master, trigger motion, wait for completion, dump report.
#
# 用法：DEPLOYMENT=<部署ID> verify_p25_motion.sh
#
# 部署 ID 取 DEPLOYMENT，其次 EMASTER_DEPLOYMENT。报告路径从部署配置的
# run_report_path 读，不写死——换部署时忘了同步会让整个校验读完一份别处的旧报告，
# 却看不出任何异常。
set -e

REPO="/home/orangepi/ethercat-master"
DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-orangepi-bench-quint-30deg}}"
PROFILE="${PROFILE:-orangepi-bench.test-dynamic-error-detection}"
SOCK="/tmp/emaster-${DEPLOYMENT}.sock"
BINARY="$REPO/build/tools/master/emaster-master"

cd "$REPO"

# 按 deployment_id 找文件，不能按文件名猜：文件名用下划线，ID 用连字符。
REPORT_REL=$(DEPLOYMENT="$DEPLOYMENT" python3 -c "
import json, os, glob, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob('$REPO/config/deployments/*.json')):
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
REPORT="$REPO/$REPORT_REL"

echo "=== 启动主站（$DEPLOYMENT）==="
sudo "$BINARY" --deployment "$DEPLOYMENT" > /tmp/emaster-verify.log 2>&1 &
MASTER_PID=$!
echo "主站 PID=$MASTER_PID"

echo "=== 等待 socket ==="
for i in $(seq 1 30); do
    if [ -S "$SOCK" ]; then
        echo "socket 就绪 (${i}s)"
        break
    fi
    sleep 1
done

if [ ! -S "$SOCK" ]; then
    echo "ERROR: socket 未出现，主站日志："
    cat /tmp/emaster-verify.log
    exit 1
fi

sleep 2  # 等待 DC 稳定

echo "=== 触发运动 ==="
RESP=$(echo "switch $PROFILE" | sudo nc -U "$SOCK")
echo "响应: $RESP"

if [[ "$RESP" != OK* ]]; then
    echo "ERROR: switch 命令失败"
    sudo kill "$MASTER_PID" 2>/dev/null || true
    exit 1
fi

echo "=== 等待运动完成 ==="
for i in $(seq 1 60); do
    STATUS=$(echo "status" | sudo nc -U "$SOCK" 2>/dev/null || echo "")
    echo "  [${i}s] $STATUS"
    if echo "$STATUS" | grep -q "completed=1"; then
        echo "运动完成！"
        break
    fi
    sleep 1
done

echo "=== 停止主站 ==="
echo "stop" | sudo nc -U "$SOCK" 2>/dev/null || true
sleep 2
sudo kill "$MASTER_PID" 2>/dev/null || true
wait "$MASTER_PID" 2>/dev/null || true

echo "=== 运行报告摘要 ==="
python3 -c "
import json
with open('$REPORT') as f:
    d = json.load(f)
r = d.get('result', {})
print('motion_started:', r.get('motion_started'))
print('motion_completed:', r.get('motion_completed'))
print('cycle_count:', r.get('cycle_count'))
print('cycle_deadline_missed:', r.get('cycle_deadline_missed'))
print('fault_latched:', r.get('fault_latched'))
for i, ax in enumerate(d.get('axes', [])):
    print(f'--- axis {i+1}: {ax.get(\"axis_id\")} ---')
    for k in ('initial_actual_position','motion_final_position',
              'motion_completion_actual_position','motion_actual_delta_counts',
              'motion_final_error_counts','max_following_error_counts',
              'max_observed_following_error_counts','motion_direction_match'):
        print(f'  {k}: {ax.get(k)}')
"
