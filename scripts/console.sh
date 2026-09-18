#!/bin/bash
# 台架手动控制台：坐到终端前，按键推电机。
#
#   sudo bash /home/orangepi/ethercat-master/scripts/console.sh           # 接上已在跑的主站
#   sudo bash /home/orangepi/ethercat-master/scripts/console.sh --start   # 没在跑就替你拉起来
#   sudo bash /home/orangepi/ethercat-master/scripts/console.sh --selftest # 不开面板，跑一遍引擎自检
#   sudo bash .../console.sh --start --obs                                # 同上，并打开观测口
#
# 为什么要在 root 下：主站要 mlockall/SCHED_FIFO，套接字由 root 创建；面板要能
# 用 m 键拉起主站也得是 root。主站已经在跑了，本脚本本身其实不需要 root。
#
# 参数（环境变量）：
#   DEPLOYMENT  部署 ID，默认 orangepi-bench-quint-30deg（台架现有 5 个从站）
#   RANGE       自助软范围，相对启动位置多少度；0 = 不限制。默认 45
#   JOG_SPEED   点动速度，输出轴度/秒，默认 10
#
# 参数（命令行）：
#   --obs       打开主站的观测通道（EMASTER_OBSERVATION=1），默认**关**。
#               关着时行为与以前逐字一致：主站不分配观测环、不建 -obs.sock，
#               报告里 observation.enabled=false。要曲线窗口（GUI）或者要
#               observation_client 取帧，就得显式加这个参数——**只对本次由本
#               脚本（或面板 m 键 / --start）拉起的主站生效**：已经在跑的主站用
#               的是它启动时那个环境，本脚本改不了它。判断依据看报告自证段。
#
# 与主站解耦：面板是独立进程，只走命令套接字，主站一行不改。退出面板时主站
# 默认继续跑，屏幕上会告诉你它的 pid。

set -u

REPO=/home/orangepi/ethercat-master
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
RANGE=${RANGE:-45}
JOG_SPEED=${JOG_SPEED:-10}

# --obs 由本脚本消费（面板不认识它），剩下的参数原样转给面板。默认关：
# 观测通道有每拍一次的组帧与发布代价，要它就得明说。
OBS=0
CONSOLE_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --obs) OBS=1 ;;
        *) CONSOLE_ARGS+=("$arg") ;;
    esac
done
if [ "$OBS" = "1" ]; then
    export EMASTER_OBSERVATION=1
    echo "观测通道：开（本次拉起的主站会建 -obs.sock）"
fi

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 root（主站要 mlockall/SCHED_FIFO，套接字由 root 创建）" >&2
    exit 1
fi

# 面板的 m 键（和 --start）是直接 spawn 主站进程的，子进程继承本 shell 的 rlimit。
# 非交互的 bash 不读 .bashrc/.profile，ulimit -r 默认是 0，主站就拿不到 SCHED_FIFO——
# 而部署声明了 realtime.required=true，它会**拒绝进入 OP**，整个臂白跑。sudo 也救不了，
# 因为这是 rlimit 不是权限（硬上限本来就是 unlimited，自己提就行）。
ulimit -r unlimited 2>/dev/null
if [ "$(ulimit -r)" != "unlimited" ]; then
    echo "警告：ulimit -r 仍是 $(ulimit -r)，主站会因为拿不到 SCHED_FIFO 拒绝进入 OP" >&2
fi

exec python3 "$REPO/tools/emaster_console.py" \
    --repo "$REPO" \
    --deployment "$DEPLOYMENT" \
    --range "$RANGE" \
    --jog-speed "$JOG_SPEED" \
    ${CONSOLE_ARGS[@]+"${CONSOLE_ARGS[@]}"}
