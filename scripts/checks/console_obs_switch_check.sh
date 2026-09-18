#!/bin/bash
# scripts/console.sh 的观测开关，离线验。
#
#   bash scripts/checks/console_obs_switch_check.sh
#
# 为什么值得单独验：这个开关的默认值是**关**，而"关"是看不见的——打开观测通道只是
# 多几个套接字和几条报告字段，默认错了不会有任何报错，只会在某一天发现"随手拉起来的
# 主站一直在多花每拍一次的组帧代价"，或者反过来，GUI 连不上观测口而屏幕上只说
# "没帧"。两种错法都不喊。所以这里钉住默认值本身，以及 `--obs` 的两条边界：
# 它得真的落到主站的环境里，又**不能**被转给不认识它的面板。
#
# 验的是**真脚本**：起一个假的 id/python3 放在 PATH 前面，真的跑一遍
# scripts/console.sh，看它 exec 出去的那个进程收到什么参数、什么环境。抄一份逻辑
# 来验，真脚本改了这边不会跟着改，检查就成了摆设。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SCRIPT=${1:-$ROOT/scripts/console.sh}

if [ ! -f "$SCRIPT" ]; then echo "找不到 $SCRIPT" >&2; exit 2; fi
if ! command -v bash >/dev/null; then echo "需要 bash" >&2; exit 2; fi

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf 'ok    %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf 'FAIL  %s\n      期望：%s\n      实际：%s\n' "$1" "$2" "$3"; }
same() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }

STUB=$(mktemp -d)
trap 'rm -rf "$STUB"' EXIT

# 假 id：root 检查过得了。假 python3：把收到的参数与观测开关打到 stdout。
cat > "$STUB/id" <<'STUB_ID'
#!/bin/sh
if [ "${1:-}" = "-u" ]; then echo 0; else echo "uid=0(root)"; fi
STUB_ID
cat > "$STUB/python3" <<'STUB_PY'
#!/bin/sh
printf 'ARGS:%s\n' "$*"
printf 'OBS:%s\n' "${EMASTER_OBSERVATION-未设}"
STUB_PY
chmod +x "$STUB/id" "$STUB/python3"

run() { PATH="$STUB:$PATH" bash "$SCRIPT" "$@" 2>/dev/null; }
args_of() { printf '%s\n' "$1" | sed -n 's/^ARGS://p'; }
obs_of()  { printf '%s\n' "$1" | sed -n 's/^OBS://p'; }

# ── 1. 默认关 ───────────────────────────────────────────────────────────────
out=$(run --start)
same "不带 --obs 时环境里没有 EMASTER_OBSERVATION（默认关）" "未设" "$(obs_of "$out")"

# ── 2. 打开 ─────────────────────────────────────────────────────────────────
out=$(run --start --obs)
same "带 --obs 时环境里是 EMASTER_OBSERVATION=1" "1" "$(obs_of "$out")"

# ── 3. --obs 不转给面板（它不认识这个参数，转过去就是启动即报错） ──────────
case " $(args_of "$out") " in
    *" --obs "*) bad "--obs 没有被转给面板" "面板参数里不含 --obs" "$(args_of "$out")" ;;
    *)           ok  "--obs 没有被转给面板（它由 console.sh 自己消费掉）" ;;
esac

# ── 4. 位置无关：--obs 在前在后，剩下的参数都原样保留 ──────────────────────
a=$(args_of "$(run --obs --start --selftest)")
b=$(args_of "$(run --start --selftest --obs)")
same "--obs 放在哪儿都等价" "$b" "$a"
case " $a " in
    *" --start "*)   ok "--obs 没吞掉它前面的参数（--start 还在）" ;;
    *)               bad "--obs 没吞掉它前面的参数" "还留着 --start" "$a" ;;
esac
case " $a " in
    *" --selftest "*) ok "--obs 没吞掉它后面的参数（--selftest 还在）" ;;
    *)                bad "--obs 没吞掉它后面的参数" "还留着 --selftest" "$a" ;;
esac

# ── 5. 不带 --obs 时，面板参数与改造前逐字一致 ─────────────────────────────
# 面板脚本自己的路径由 console.sh 的 REPO 决定，不参与判定，先削掉。
out=$(run --selftest)
same "不带 --obs 时参数原样转发（多个参数、顺序不变）" \
     "--repo /home/orangepi/ethercat-master --deployment orangepi-bench-quint-30deg --range 45 --jog-speed 10 --selftest" \
     "$(args_of "$out" | sed 's|^[^ ]*/tools/emaster_console.py ||')"

echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
