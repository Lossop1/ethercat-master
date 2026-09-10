#!/bin/bash
set -e

cd ~/ethercat-master

# 清理
sudo pkill -9 emaster-master 2>/dev/null || true
rm -f /tmp/emaster.log

# 启动主站（无运动配置）
echo "Starting master..."
echo orangepi | sudo -S build/tools/master/emaster-master --deployment orangepi-bench-dual > /tmp/emaster.log 2>&1 &
MASTER_PID=$!

# 等待 socket 创建
SOCK=/tmp/emaster-orangepi-bench-dual.sock
for i in {1..30}; do
  if [ -S "$SOCK" ]; then
    echo "Socket found"
    break
  fi
  sleep 1
done

if [ ! -S "$SOCK" ]; then
  echo "Socket not created, master failed"
  tail -50 /tmp/emaster.log
  sudo pkill -9 emaster-master
  exit 1
fi

# 等待主站进入 OPERATIONAL 状态
sleep 5

# 发送 switch 命令
echo "Sending switch command..."
echo "switch test-switch-small" | sudo nc -U "$SOCK"

# 等待执行
sleep 8

# 检查主站是否还在运行
if pgrep -x emaster-master > /dev/null; then
  echo "Master still running - GOOD"
else
  echo "Master crashed - BAD"
fi

# 输出日志
echo "=== Master log ==="
tail -100 /tmp/emaster.log

# 清理
sudo pkill -9 emaster-master 2>/dev/null || true
