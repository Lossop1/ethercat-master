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
# 匹配前先去掉注释、字符串字面量和字符常量。文件里提一句函数名（说明文字、
# 参数举例）不是调用点，登记进去只会让名单变长，真出现新调用点时反而看不出来。
#
# 维护方式：往 ALLOWED 里加文件时，同时更新 session_internal.h 顶部那段说明
# 里的线程归属表，两者必须一致。

set -euo pipefail

# 本脚本在 <仓库>/tests/checks/ 下，仓库根是它上一级。目录重排时这一行写成了 /../..，
# 于是 cd 到了仓库的上一级：grep 找不到 src/，found 为空，ALLOWED 里每一行都被
# 判成"已不再调用 SOEM"，检查恒失败、也就恒发不出信号。
cd "$(dirname "$0")/../.."

# 与 session_internal.h 的线程归属表一一对应：
#   周期线程（RT）      session_exchange.c
#   观测线程（P4.3）    session_observer_thread.c
#   会话线程（周期外）  session_setup.c / session_start.c / session_shutdown.c
#                       dc_prepare.c / preop_probe.c / session_observer.c
#   多线程共用          soem_common.c（薄封装，调用方决定线程）
#   独立进程（不在会话里）apps/diagnose_slave/main.c / apps/disable_drives/main.c
#                       —— 各自 ecx_init 自己的 context，用完 ecx_close 退出，与
#                       周期线程不共存；安全靠的是进程隔离而不是锁，所以要登记、
#                       要被看见。
ALLOWED="
src/bus/soem/dc_prepare.c
src/bus/soem/preop_probe.c
src/bus/session/session_exchange.c
src/bus/observe/session_observer.c
src/bus/observe/session_observer_thread.c
src/bus/session/session_setup.c
src/bus/session/session_shutdown.c
src/bus/session/session_start.c
src/bus/soem/soem_common.c
apps/diagnose_slave/main.c
apps/disable_drives/main.c
"

# 去掉注释、字符串字面量与字符常量。块注释跨行，所以不能按行过滤。
# 这段若失效：去过头会让下面第二段检查（ALLOWED 里的文件已不再调用 SOEM）
# 全量报错，去不够会让上面第一段检查报出注释里的假调用点——两头都是硬失败，
# 不会静默放过。
strip_comments() {
    awk '
    BEGIN { in_block = 0 }
    {
        line = $0
        out = ""
        i = 1
        n = length(line)
        while (i <= n) {
            two = substr(line, i, 2)
            ch = substr(line, i, 1)
            if (in_block) {
                if (two == "*/") { in_block = 0; i += 2 } else { i++ }
                continue
            }
            if (two == "/*") { in_block = 1; i += 2; continue }
            if (two == "//") break
            if (ch == "\"" || ch == "\047") {
                out = out ch
                i++
                while (i <= n) {
                    ch = substr(line, i, 1)
                    out = out ch
                    if (ch == "\\") {
                        i++
                        if (i <= n) { out = out substr(line, i, 1) }
                    } else if (ch == "\"" || ch == "\047") {
                        i++
                        break
                    }
                    i++
                }
                continue
            }
            out = out ch
            i++
        }
        print out
    }' "$1"
}

# 认的是调用写法（标识符后跟左括号）。
CALL_PATTERN='\becx_[a-zA-Z0-9_]+ *\('

found=""
while IFS= read -r file; do
    [ -n "$file" ] || continue
    real=$(strip_comments "$file" | grep -E "$CALL_PATTERN" || true)
    if [ -n "$real" ]; then
        found="${found}${file}"$'\n'
    fi
done <<<"$(grep -rlE "$CALL_PATTERN" src/ include/ tools/ tests/ apps/ 2>/dev/null | sort || true)"

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
