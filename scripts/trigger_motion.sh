#!/bin/bash
# 通过 socket 触发 motion profile 执行。
#
# 用法：trigger_motion.sh [socket路径] [motion profile ID]
#
# socket 路径不写死：不给第一个参数时按部署 ID 推导，规则与主站一致
# （/tmp/emaster-<deployment_id>.sock）。部署 ID 取 DEPLOYMENT，其次
# EMASTER_DEPLOYMENT。此前这里写死双轴部署，换了台架
# 接线就会连到一个不存在的主站上，却只报"socket 文件不存在"。

DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-}}"
if [ -z "$DEPLOYMENT" ]; then
  echo "必须显式给 DEPLOYMENT=<你的部署ID>：部署 ID 是现场生成的，仓库里没有默认值（README.md 第 20 节）" >&2
  exit 2
fi
SOCKET="${1:-/tmp/emaster-${DEPLOYMENT}.sock}"
MOTION_PROFILE="${2:-}"
if [ -z "$MOTION_PROFILE" ]; then
  echo "必须给 motion profile ID：$0 [socket路径] <motion_profile_id>" >&2
  exit 2
fi

if [ ! -S "$SOCKET" ]; then
    echo "错误: socket文件不存在: $SOCKET" >&2
    exit 1
fi

echo "motion $MOTION_PROFILE" | nc -U "$SOCKET"
