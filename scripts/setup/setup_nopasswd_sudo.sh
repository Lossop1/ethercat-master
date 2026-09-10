#!/bin/bash
# 为 orangepi 用户配置全局免密 sudo

echo "Configuring NOPASSWD sudo for user orangepi..."

# 创建 sudoers 配置文件
echo 'orangepi ALL=(ALL) NOPASSWD: ALL' | sudo tee /etc/sudoers.d/orangepi-nopasswd

# 设置正确的权限
sudo chmod 440 /etc/sudoers.d/orangepi-nopasswd

# 验证配置
if sudo visudo -c -f /etc/sudoers.d/orangepi-nopasswd; then
    echo "✓ Configuration successful"
    echo "Testing sudo without password..."
    sudo whoami
    echo "Done. You can now use sudo without password."
else
    echo "✗ Configuration failed - syntax error"
    sudo rm -f /etc/sudoers.d/orangepi-nopasswd
    exit 1
fi
