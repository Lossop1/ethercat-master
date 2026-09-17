#!/usr/bin/env bash
#
# P9.1 防回退检查：哪些文件直接调用 SOEM，必须在本清单里登记。
#
# 为什么需要它。session_internal.h 里那段"SOEM 访问如何串行化"的说明是本项目的
# 一条架构约束，但它不是一个编译器或断言能抓住的性质。只有"哪些文件在调用
# SOEM"这件事是机械可查的：多出一个调用点，就可能多一个线程在与周期线程共用
# 同一个 context 与 socket，必须由人确认之后再放行。所以这里查的是**名单**，
# 不是行为——名单对不上就拦下来问一句。
#
# 维护方式：往 ALLOWED 里加文件时，同时更新 session_internal.h 顶部那段说明
# 里的线程归属表，两者必须一致。

set -euo pipefail

cd "$(dirname "$0")/../.."

# 与 session_internal.h 的线程归属表一一对应：
#   周期线程（RT）      session_exchange.c
#   观测线程（P4.3）    session_observer_thread.c
#   会话线程（周期外）  session_setup.c / session_start.c / session_shutdown.c
#                       dc_prepare.c / preop_probe.c / session_observer.c
#   多线程共用          soem_common.c（薄封装，调用方决定线程）
#   类型与说明          session_internal.h（只有 ecx_contextt 字段声明）
ALLOWED="
src/bus/soem/dc_prepare.c
src/bus/soem/preop_probe.c
src/bus/soem/session_exchange.c
src/bus/soem/session_internal.h
src/bus/soem/session_observer.c
src/bus/soem/session_observer_thread.c
src/bus/soem/session_setup.c
src/bus/soem/session_shutdown.c
src/bus/soem/session_start.c
src/bus/soem/soem_common.c
"

# 只认真正的调用写法（标识符后跟左括号），不认注释里提一句函数名。
found=$(grep -rlE '\becx_[a-zA-Z0-9_]+ *\(' src/ include/ tools/ tests/ 2>/dev/null | sort || true)

status=0

while IFS= read -r file; do
    [ -n "$file" ] || continue
    if ! grep -qxF "$file" <<<"$ALLOWED"; then
        echo "未登记的 SOEM 调用点：$file" >&2
        echo "  先确认它在哪个线程上调用、会不会与周期线程共用同一个 context/socket，" >&2
        echo "  把结论写进 session_internal.h 的说明，再把文件加进本脚本的 ALLOWED。" >&2
        status=1
    fi
done <<<"$found"

while IFS= read -r file; do
    [ -n "$file" ] || continue
    if ! grep -qxF "$file" <<<"$found"; then
        echo "ALLOWED 里的文件已不再调用 SOEM：$file" >&2
        echo "  删掉这一行，保持名单与实际一致。" >&2
        status=1
    fi
done <<<"$ALLOWED"

if [ "$status" -eq 0 ]; then
    echo "SOEM 调用点检查通过：$(grep -c . <<<"$found") 个文件，全部已登记。"
fi

exit "$status"
