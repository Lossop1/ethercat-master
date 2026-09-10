#!/bin/bash
# EtherCAT主站启动脚本 - 确保单实例运行

set -e

DEPLOYMENT="${1:-orangepi-bench-dual}"
BUILD_DIR="${BUILD_DIR:-$HOME/ethercat-master/build}"
MASTER_BIN="$BUILD_DIR/tools/master/emaster-master"
LOG_FILE="${LOG_FILE:-/tmp/master.log}"
SOCKET_PATH="/tmp/emaster-${DEPLOYMENT}.sock"

# 检查主站可执行文件是否存在
if [ ! -x "$MASTER_BIN" ]; then
    echo "错误：找不到主站可执行文件：$MASTER_BIN"
    echo "请先编译主站：cd $BUILD_DIR && make"
    exit 1
fi

# 检查是否已有主站在运行
if pgrep -f "emaster-master --deployment $DEPLOYMENT" > /dev/null; then
    echo "警告：检测到主站已在运行（deployment=$DEPLOYMENT）"
    read -p "是否停止现有主站并重新启动？[y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "正在停止现有主站..."
        sudo pkill -9 -f "emaster-master --deployment $DEPLOYMENT"
        sleep 2
    else
        echo "取消启动"
        exit 0
    fi
fi

# 清理遗留的socket文件
if [ -S "$SOCKET_PATH" ]; then
    echo "清理遗留的socket文件：$SOCKET_PATH"
    sudo rm -f "$SOCKET_PATH"
fi

# 启动主站
echo "启动EtherCAT主站..."
echo "  Deployment: $DEPLOYMENT"
echo "  日志文件: $LOG_FILE"
echo "  Socket路径: $SOCKET_PATH"
echo

sudo "$MASTER_BIN" --deployment "$DEPLOYMENT" > "$LOG_FILE" 2>&1 &
MASTER_PID=$!

# 等待启动
echo "等待主站启动..."
for i in {1..10}; do
    if [ -S "$SOCKET_PATH" ]; then
        echo "✓ 主站已启动（PID=$MASTER_PID）"
        echo "✓ Socket已创建：$SOCKET_PATH"
        echo
        echo "查看日志：tail -f $LOG_FILE"
        echo "发送命令：sudo ~/ethercat-master/build/set_target_client $SOCKET_PATH <pos1> <pos2> ..."
        exit 0
    fi
    sleep 0.5
done

echo "错误：主站启动超时，socket未创建"
echo "请检查日志：tail $LOG_FILE"
exit 1
