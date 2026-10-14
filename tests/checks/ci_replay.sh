#!/usr/bin/env bash
# 按 .github/workflows/ci.yml 的步骤顺序，在**本机**跑一遍同一批检查，逐条记录退出码。
# 用途：不依赖 GitHub 就知道 CI 会不会绿；也用来验一棵干净的检出。
#
# 与 ci.yml 是复制关系：ci.yml 改了这里也要改。tests/checks/delivery_consistency_check.py
# 会比对两边跑的检查集合，漏一边就判红。
#
# 依赖 ninja 与 cppcheck（与 CI 的 ubuntu-24.04 一致）。
# 用法：bash ci_replay.sh [树目录]，缺省 /tmp/verify。必须指向一棵干净的检出。
cd "${1:-/tmp/verify}" || exit 1

fails=0
run() {
    name="$1"; shift
    if "$@" > /tmp/ci_step.log 2>&1; then
        echo "PASS  $name"
    else
        rc=$?
        echo "FAIL  $name (rc=$rc)"
        tail -8 /tmp/ci_step.log | sed 's/^/      /'
        fails=$((fails + 1))
    fi
}

run "校验配置"                 python3 tools/gen/validate_project.py
run "校验 SOEM 调用点"         bash tests/checks/soem_call_sites.sh
run "主站线程调度（离线）"     python3 tests/checks/master_thread_sched_check.py
run "界面证据判定（离线）"     python3 tests/checks/gui_evidence_check.py
run "绑核缓存（离线）"         python3 tests/checks/rt_affinity_check.py
run "控制台观测开关（离线）"   bash tests/checks/console_obs_switch_check.sh
run "命令口目标入口不受设点开关管（离线）" python3 tests/checks/target_entry_check.py
run "目标入口检查的判别力"     python3 tests/checks/target_entry_check.py --mutate
run "报告慢速遥测渲染（离线）" python3 tests/checks/report_telemetry_check.py
run "报告库源文件清单与闭包一致（离线）" python3 tests/checks/report_sources_closure.py
run "分析工具不得硬编码（离线）" python3 tests/checks/analysis_tools_check.py
run "硬编码路径的判别力"       python3 tests/checks/analysis_tools_check.py --mutate
run "命令口停机走安全门（离线）" python3 tests/checks/stop_path_check.py
run "停机路径检查的判别力"     python3 tests/checks/stop_path_check.py --mutate
run "停机结论的三档与失能字段名（离线）" python3 tests/checks/shutdown_verdict_check.py
run "停机结论检查的判别力"     python3 tests/checks/shutdown_verdict_check.py --mutate
run "丢失链计数读得到、落得进报告（离线）" python3 tests/checks/lost_link_counter_check.py
run "丢失链计数检查的判别力"   python3 tests/checks/lost_link_counter_check.py --mutate
run "设点通道的线格式与只写语义（离线）" python3 tests/checks/setpoint_channel_check.py
run "设点通道检查的判别力"     python3 tests/checks/setpoint_channel_check.py --mutate
run "慢速遥测渲染的判别力"     python3 tests/checks/report_telemetry_check.py --mutate
run "报告故障字段的 JSON 合法性" python3 tests/checks/report_axis_fault_check.py
run "故障字段检查的判别力"     python3 tests/checks/report_axis_fault_check.py --mutate
run "归档削旧与删除留声（离线）" python3 tests/checks/report_archive_check.py
run "削旧检查的判别力"         python3 tests/checks/report_archive_check.py --mutate
run "装置指纹符合 schema（离线）" python3 tests/checks/fingerprint_schema_check.py
run "设备档推导链（离线）"     python3 tests/checks/device_facts_check.py
run "生效 PDO 的两种分配方式（离线）" python3 tests/checks/pdo_assignment_check.py
run "配置引用链（离线）"       python3 tests/checks/config_chain_check.py
run "运动方案挂载链（离线）"   python3 tests/checks/motion_attach_check.py
run "控制台引擎回归（离线）"   python3 tests/checks/console_engine_check.py --selftest
run "启动窗口检查的判别力"     python3 tests/checks/console_engine_check.py --mutate-startup-window
run "交付面一致性（离线）"     python3 tests/checks/delivery_consistency_check.py
run "交付面一致性检查的判别力" python3 tests/checks/delivery_consistency_check.py --mutate

# 客户入口出配置：空配置源要能生成出四族扁平文件，且与入库的 config-runtime 逐字节一致。
rm -rf /tmp/ci_runtime
if python3 tools/gen/emaster_config.py build --config config --out /tmp/ci_runtime > /tmp/ci_step.log 2>&1 \
   && diff -r config-runtime /tmp/ci_runtime >> /tmp/ci_step.log 2>&1; then
    echo "PASS  客户入口出配置（离线）"
else
    echo "FAIL  客户入口出配置（离线）"
    tail -8 /tmp/ci_step.log | sed 's/^/      /'
    fails=$((fails + 1))
fi

echo "----- 构建前步骤：fails=$fails -----"

run "配置（linux-debug 预设）"  cmake --preset linux-debug
run "构建（linux-debug 预设）"  cmake --build --preset linux-debug --parallel 2
run "可交付入口的 --help 与 --version" bash tests/checks/cli_contract_check.sh build/linux-debug
run "模式集合的运行期校核（离线）" python3 tests/checks/mode_gate_check.py build/linux-debug
run "目标量的断供语义（离线）"  python3 tests/checks/mode_target_check.py build/linux-debug
run "观测通道离线自测"          ./build/linux-debug/apps/observation_selftest/observation-selftest
run "单元测试"                  ctest --test-dir build/linux-debug --output-on-failure
run "静态分析"                  cppcheck --enable=warning,performance,portability --error-exitcode=1 --std=c11 --suppress=missingIncludeSystem -I include -I build/linux-debug/generated src/catalog src/bus/soem src/bus/observation apps/fingerprint

echo "----- CI 全序列：fails=$fails -----"
exit $((fails > 0))

