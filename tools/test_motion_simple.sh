#!/bin/bash
# 简单的运动测试：假设主站已经在运行，切一个 motion profile 再看十秒状态。
#
# 用法：DEPLOYMENT=<部署ID> test_motion_simple.sh
#
# 部署 ID 取 DEPLOYMENT，其次 EMASTER_DEPLOYMENT。此前这里写死带 motion 的双轴
# 部署，换成别的部署只会看到"socket 不存在"，看不出是路径写死了。
set -e

DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-orangepi-bench-quint-30deg}}"
SOCK="/tmp/emaster-${DEPLOYMENT}.sock"
PROFILE="${PROFILE:-orangepi-bench.test-dynamic-error-detection}"

if [ ! -S "$SOCK" ]; then
    echo "错误：主站未运行或 socket 不存在（$SOCK）" >&2
    exit 1
fi

echo "=== 触发运动 ==="
RESP=$(echo "switch $PROFILE" | sudo nc -U "$SOCK")
echo "响应: $RESP"

echo "=== 等待 10 秒并查询状态 ==="
for i in $(seq 1 10); do
    STATUS=$(echo "status" | sudo nc -U "$SOCK" 2>/dev/null || echo "")
    echo "  [${i}s] $STATUS"
    sleep 1
done

echo "=== 完成 ==="
