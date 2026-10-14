#!/usr/bin/env bash
# 每个可交付程序都要认 --help 与 --version：都打到 stdout，都以 0 退出。
#
# 为什么单独一条：这两条谁也碰不到。漏掉 --version 的程序照常干活，漏掉 --help 的
# 只在人照着文档试的时候才发现；而"退出码是 2 还是 0"连跑的人都不会看一眼。
# 用户侧看到的是"这个工具没有版本号" / "问它怎么用，它却报了个错"。
#
# 用法：bash tests/checks/cli_contract_check.sh [构建目录]，默认 build
#
# 名单是交付面的十个（README 第 8 节的工具表）。observation-selftest 不在此列：
# 它是验证手段，不进 install，README 里也写明不是交付物。

set -u
BUILD="${1:-build}"

APPS=(
    master/emaster-master
    diagnose_slave/emaster-scan
    disable_drives/emaster-disable
    emaster_watch/emaster-watch
    emaster_move/emaster-move
    emaster_move_deg/emaster-move-deg
    fingerprint/emaster-fingerprint
    dc_prepare/emaster-dc-prepare
    target_probe/emaster-target-probe
    status_client/emaster-status
)

fail=0
for app in "${APPS[@]}"; do
    binary="$BUILD/apps/$app"
    if [ ! -x "$binary" ]; then
        echo "缺可执行：$binary"
        fail=1
        continue
    fi
    for flag in --help --version; do
        # 只看 stdout：人读的用法出现在 stderr 上就不算数（管道接不到）。
        # 3 秒足够 —— 这两个开关都在建会话之前返回，卡住说明它去干活了。
        output=$(timeout 3 "$binary" "$flag" 2>/dev/null)
        rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "$binary $flag：退出码 $rc，期望 0"
            fail=1
        elif [ -z "$output" ]; then
            echo "$binary $flag：stdout 为空"
            fail=1
        fi
    done
done

if [ "$fail" -eq 0 ]; then
    echo "可交付入口 ${#APPS[@]} 个：--help 与 --version 都可用（stdout，退出 0）"
fi
exit "$fail"
