#!/bin/bash
# 持续运动能力测试脚本
set -e

cd ~/ethercat-master

# 清理
sudo pkill -9 emaster-master 2>/dev/null || true
rm -f /tmp/emaster.log

# 启动主站
echo "启动主站（配置：orangepi-current-bench，持续运动测试）..."
sudo build/tools/master/emaster-master --deployment orangepi-current-bench > /tmp/emaster.log 2>&1 &
MASTER_PID=$!
echo "主站 PID: $MASTER_PID"

# 等待 socket 创建
SOCK=/tmp/emaster-orangepi-current-bench.sock
echo "等待 socket $SOCK 创建..."
for i in {1..30}; do
  if [ -S "$SOCK" ]; then
    echo "✓ Socket 已创建"
    break
  fi
  sleep 1
done

if [ ! -S "$SOCK" ]; then
  echo "✗ Socket 未创建，主站启动失败"
  tail -50 /tmp/emaster.log
  sudo pkill -9 emaster-master
  exit 1
fi

# 等待主站进入 OPERATIONAL 状态
echo "等待主站进入 OPERATIONAL..."
sleep 5

# 启动持续运动测试（60秒）
echo "发送持续运动命令（60秒）..."
echo "switch orangepi-bench.continuous-motion-test" | sudo nc -U "$SOCK"

# 监控运行状态
echo "监控运行状态（60秒）..."
for i in {1..12}; do
  sleep 5
  if pgrep -x emaster-master > /dev/null; then
    echo "[${i}x5s] 主站运行中..."
  else
    echo "✗ 主站崩溃"
    break
  fi
done

# 等待运动完成
sleep 5

# 检查主站状态
if pgrep -x emaster-master > /dev/null; then
  echo "✓ 主站仍在运行"
else
  echo "✗ 主站已停止"
fi

# 输出最后 200 行日志
echo ""
echo "=== 主站日志（最后200行）==="
tail -200 /tmp/emaster.log

# 清理
sudo pkill -9 emaster-master 2>/dev/null || true

echo ""
echo "测试完成"
