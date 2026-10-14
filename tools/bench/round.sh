#!/bin/bash
# 台架：跑一轮完整会话，并且只用产品自己的停机路径收尾。
#
# 一轮 = 起主站 → 等它报 RUNNING → 执行给定的那条命令 → SIGINT → 打印报告摘要。
# 每一轮都是独立的一次会话，所以每轮都留一份报告；多轮连跑才能看见随时间才出现的问题。
#
# 用法:
#   round.sh <配置目录> <部署ID> [-- <要在会话里执行的命令...>]
#
# 约定：命令执行时的环境里带 SOCK（命令套接字路径）与 AXES（轴数）。
# 不给命令就是纯空转一轮，用来看静置行为。
# 环境变量 DWELL 秒数：命令跑完后再等这么久才停机，用来看持续时间里的行为。
# 默认 0，即命令一结束就停机。
#
# 前置：部署树里已有 config-runtime，主站二进制已构建。本体不设任何电机默认值。
set -u

CONFIG_DIR=${1:-}
DEPLOYMENT=${2:-}
if [ -z "$CONFIG_DIR" ] || [ -z "$DEPLOYMENT" ]; then
  echo "用法: round.sh <配置目录> <部署ID> [-- <命令...>]" >&2
  exit 2
fi
shift 2
[ "${1:-}" = "--" ] && shift

WORKDIR=$(cd "$CONFIG_DIR/.." && pwd)
MASTER=${MASTER:-$HOME/ethercat-master/build/apps/master/emaster-master}
LOG=${LOG:-$WORKDIR/runtime/round.log}
SOCK=/tmp/emaster-$DEPLOYMENT.sock
ROUND=${ROUND:-手动}

for tool in "$MASTER" "$SOCK"; do :; done
[ -x "$MASTER" ] || { echo "找不到主站二进制：$MASTER（用 MASTER= 覆盖）" >&2; exit 2; }

mkdir -p "$WORKDIR/runtime"
: > "$LOG"
cd "$WORKDIR" || exit 2

echo "== 轮次 $ROUND：部署 $DEPLOYMENT，配置 $CONFIG_DIR =="
START=$(date +%s)
nohup sudo -n bash -c "ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 EMASTER_SETPOINT=1 \
  '$MASTER' --config-dir '$CONFIG_DIR' --deployment '$DEPLOYMENT'" \
  >> "$LOG" 2>&1 &
sleep 1

STOP=0
trap 'STOP=1' INT TERM

# 等命令口出现，再等它报 RUNNING。两条分开：套接字存在不代表会话起来了。
for _ in $(seq 1 60); do
  [ -S "$SOCK" ] && break
  sleep 0.5
done
if [ ! -S "$SOCK" ]; then
  echo "命令套接字没出现，主站日志尾部："
  tail -20 "$LOG"
  sudo -n pkill -INT -x emaster-master
  exit 1
fi

# 等 RUNNING。失败时把最后一次原始回应与日志尾部都打出来：只说"没等到"没法判是
# 主站没起来、状态值不对，还是套接字连不上——这三种要修的完全不是一回事。
READY=$(python3 - "$SOCK" <<'EOF'
import socket, sys, time
path = sys.argv[1]
deadline = time.monotonic() + 60
last = "(一次都没读回来)"
while time.monotonic() < deadline:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(path)
    except OSError as error:
        last = "连接失败: %s" % error
        time.sleep(1)
        continue
    try:
        s.sendall(b"status\n")
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        line = buf.split(b"\n")[0].decode("utf-8", "replace")
    except OSError as error:
        line = "读回应失败: %s" % error
    finally:
        s.close()
    last = line
    if line.startswith("OK|"):
        fields = dict(item.split("=", 1) for item in line[3:].split("|")[0].split() if "=" in item)
        if fields.get("state") == "4":
            print("RUNNING " + fields.get("axes", "0"))
            raise SystemExit(0)
    time.sleep(1)
print("未就绪 " + last)
raise SystemExit(1)
EOF
)
AXES=${READY#* }
if [ "${READY%% *}" != "RUNNING" ]; then
  echo "主站没有在 60 秒内进入 RUNNING：${READY#* }"
  echo "日志尾部："
  tail -30 "$LOG"
  sudo -n pkill -INT -x emaster-master
  exit 1
fi
echo "主站 RUNNING，轴数 $AXES，用时 $(( $(date +%s) - START )) 秒"

export SOCK AXES
if [ $# -gt 0 ]; then
  echo "-- 命令：$* --"
  "$@"
  RC=$?
  echo "-- 命令退出码 $RC --"
fi

DWELL=${DWELL:-0}
if [ "$DWELL" -gt 0 ]; then
  echo "-- 停机前再跑 $DWELL 秒 --"
  sleep "$DWELL"
fi

# 正常停机：SIGINT 走的是和命令口 shutdown 完全相同的安全停机路径。
echo "-- SIGINT --"
sudo -n pkill -INT -x emaster-master
for _ in $(seq 1 50); do
  pgrep -x emaster-master > /dev/null || break
  sleep 0.5
done
if pgrep -x emaster-master > /dev/null; then
  echo "主站 25 秒还没退出——不正常，停在这里等人看，不用 SIGKILL"
  exit 1
fi
echo "主站已退出，本轮用时 $(( $(date +%s) - START )) 秒"

# 报告由主站以 root 写出，reports/ 对普通用户不可读，所以摘要在 sudo 下跑。
sudo -n python3 - "$DEPLOYMENT" "$WORKDIR" "$LOG" <<'EOF'
import glob, json, os, sys
deployment, workdir, log = sys.argv[1], sys.argv[2], sys.argv[3]
reports = sorted(glob.glob(os.path.join(workdir, "runtime", "reports", "*%s*.json" % deployment)),
                 key=os.path.getmtime)
if not reports:
    print("没有报告文件")
    raise SystemExit(0)
path = reports[-1]
d = json.load(open(path))
print("报告:", os.path.basename(path), "schema", d.get("schema_version"))
print("  result:", json.dumps(d.get("result"), ensure_ascii=False)[:400])
print("  process_data_delivery:", json.dumps(d.get("process_data_delivery"), ensure_ascii=False)[:300])
print("  first_cycle_failure:", json.dumps(d.get("first_cycle_failure"), ensure_ascii=False)[:200])
print("  timing_excursions:", json.dumps(d.get("timing_excursions"), ensure_ascii=False)[:200])
print("  线程:", [(t.get("policy"), t.get("priority"), t.get("cpus_allowed"))
                 for t in d.get("thread_schedstat", [])])
for axis in d.get("axes", []):
    op = axis.get("operation", {})
    print("  轴%d: mode=%s 停机=%s" % (axis.get("position", 0), op.get("mode_id"),
          json.dumps(axis.get("shutdown"), ensure_ascii=False)[:200]))
EOF

echo "日志尾部："
tail -5 "$LOG"
