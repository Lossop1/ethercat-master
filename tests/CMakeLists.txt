# 离线单元测试。
#
# 这些测试不是覆盖率填充：每一条都复刻了一次现场故障，并在两个方向上钉住判别力
# （既要抓住真故障，也要放过健康的长跑）。此前它们只活在文件头部的手工编译命令
# 里，因此从未在 CI 上跑过——判据不进 CI 就等于没有判据。
#
# 全部不依赖 SOEM、不访问总线，因此在开发机与 CI 上都能跑。硬件相关的验证仍然
# 只能在台架上做，那部分不进这里。

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 走 clock_nanosleep 的绝对时基等待，MinGW 上没有这个调用，因此只在 Linux 上建。
    add_executable(test_recovery_gap
        unit/cycle_clock/test_recovery_gap.c
        ${CMAKE_SOURCE_DIR}/src/cyclic/clock.c)
    target_include_directories(test_recovery_gap PRIVATE ${CMAKE_SOURCE_DIR}/src)
    emaster_enable_strict_warnings(test_recovery_gap)
    add_test(NAME recovery_gap COMMAND test_recovery_gap)
endif()

# 纯逻辑，不碰时钟也不碰 SOEM：任何平台都能建、都能跑。
add_executable(test_window unit/cyclic/test_window.c)
target_link_libraries(test_window PRIVATE emaster::cyclic)
emaster_enable_strict_warnings(test_window)
add_test(NAME cyclic_window COMMAND test_window)

# 审计容量两段语义：解封之后容量用尽只能计数，不许再分配（P8.5 的修复点）。
add_executable(test_audit_capacity unit/audit/test_audit_capacity.c)
target_link_libraries(test_audit_capacity PRIVATE emaster::run_audit)
emaster_enable_strict_warnings(test_audit_capacity)
add_test(NAME audit_capacity COMMAND test_audit_capacity)

# 四族装载的回合测试：把 config-runtime 的扁平文件装进结构体，逐条 dump 出来，
# 并核对装载条数与文件声明的 count 一致。跑的是交付时的那套 flat —— 空配置也必须
# 装得进去（客户拿到仓库后的第一件事就是空跑一次），所以这里不假定任何条目存在。
add_executable(test_topology_roundtrip topology_apply_roundtrip.c)
target_link_libraries(test_topology_roundtrip PRIVATE emaster::config)
emaster_enable_strict_warnings(test_topology_roundtrip)
add_test(NAME topology_roundtrip
         COMMAND test_topology_roundtrip ${CMAKE_SOURCE_DIR}/config-runtime/topologies.flat)

add_executable(test_catalog_roundtrip catalog_apply_roundtrip.c)
target_link_libraries(test_catalog_roundtrip PRIVATE emaster::config emaster::catalog)
emaster_enable_strict_warnings(test_catalog_roundtrip)
add_test(NAME catalog_roundtrip
         COMMAND test_catalog_roundtrip ${CMAKE_SOURCE_DIR}/config-runtime/catalog.flat)

add_executable(test_runtime_roundtrip runtime_apply_roundtrip.c)
target_link_libraries(test_runtime_roundtrip PRIVATE emaster::config)
emaster_enable_strict_warnings(test_runtime_roundtrip)
add_test(NAME runtime_roundtrip
         COMMAND test_runtime_roundtrip ${CMAKE_SOURCE_DIR}/config-runtime/runtime.flat)

# 部署族要另外三族已经在场：它按 ID 去查前者的指针。
add_executable(test_deployment_roundtrip deployment_apply_roundtrip.c)
target_link_libraries(test_deployment_roundtrip PRIVATE emaster::config emaster::catalog)
emaster_enable_strict_warnings(test_deployment_roundtrip)
add_test(NAME deployment_roundtrip
         COMMAND test_deployment_roundtrip
                 ${CMAKE_SOURCE_DIR}/config-runtime/topologies.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/catalog.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/runtime.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/deployments.flat)

# 扁平文件读器：键名查找、类型转换、缺失语义。样本由测试自己写，不问任何型号的键。
add_executable(test_config_reader_smoke config_reader_smoke.c)
target_link_libraries(test_config_reader_smoke PRIVATE emaster::config)
emaster_enable_strict_warnings(test_config_reader_smoke)
add_test(NAME config_reader_smoke
         COMMAND test_config_reader_smoke ${CMAKE_CURRENT_BINARY_DIR}/config-reader-smoke.flat)

# 公开访问函数与扁平文件逐族对照。期望值从被交给它的那几个文件里读，不写死型号 ——
# 交付时跑的是空的 config-runtime，问的是"空配置是合法的可装载状态"；客户自己生成
# 一套之后同一个测试问的是"访问函数报的和文件里写的一致"。换电机、换网卡都不用改它。
add_executable(test_public_api_swap public_api_swap.c)
target_link_libraries(test_public_api_swap PRIVATE emaster::config emaster::catalog)
emaster_enable_strict_warnings(test_public_api_swap)
add_test(NAME public_api_swap
         COMMAND test_public_api_swap
                 ${CMAKE_SOURCE_DIR}/config-runtime/topologies.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/catalog.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/runtime.flat
                 ${CMAKE_SOURCE_DIR}/config-runtime/deployments.flat)

# CiA 402 状态字解码的回归基线。它不断言"解码是对的"，而是钉住**当前已知的行为**：
# 掩码 0x006F 下，真使能时报 0x1250 的驱动器会被解码成 switch-on-disabled，
# 于是停机判据判它"已安全"。这是台架实测过的反常识事实，本轮不改状态机，只固化基线
# —— 谁哪天动掩码或映射，这个测试先红，逼他先读 docs/test-plan.md §2.1。
add_executable(test_position_scale_source unit/motion/test_position_scale_source.c)
target_link_libraries(test_position_scale_source PRIVATE emaster::motion emaster::config)
emaster_enable_strict_warnings(test_position_scale_source)
add_test(NAME position_scale_source COMMAND test_position_scale_source)

add_executable(test_cia402_status_baseline unit/cia402/test_status_baseline.c)
target_link_libraries(test_cia402_status_baseline PRIVATE emaster::cia402)
emaster_enable_strict_warnings(test_cia402_status_baseline)
add_test(NAME cia402_status_baseline COMMAND test_cia402_status_baseline)

# 慢速遥测"读到过没有、多久以前读到的"：报告里那几个 0 该印成读数还是印「—」全靠
# 这一条判定，而判错了不会有任何报错，只会安静地把"没读到"写成温度 0（P11.8）。
# 只依赖 slow.c（纯 libc），所以开发机上也能建、能跑。
add_executable(test_slow_age unit/observation/test_slow_age.c)
target_link_libraries(test_slow_age PRIVATE emaster::observation_slow)
emaster_enable_strict_warnings(test_slow_age)
add_test(NAME slow_age COMMAND test_slow_age)

# 外部目标路径上唯一保留的判据：|目标 - 实际位置| 与逐轴上限。
# 源文件直接编进来，不链 emaster::motion —— 这个判据的前提就是它脱离会话也能验证，
# 一旦它开始需要配置库或总线类型才能编译，那个前提就已经没了。
add_executable(test_target_guard
    unit/motion/test_target_guard.c
    ${CMAKE_SOURCE_DIR}/src/motion/target_guard.c)
target_include_directories(test_target_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)
emaster_enable_strict_warnings(test_target_guard)
add_test(NAME target_guard COMMAND test_target_guard)

# 设点记录的线格式：主站与外部控制器之间唯一的书面契约。
#
# 只依赖 wire.c（纯 libc），所以开发机上也能建、能跑。这一点要紧：这一层算错一个
# 字节偏移不会有任何报错，只会让主站照着一串错位但"看起来合理"的位置计数驱动电机，
# 而那种错只在台架上看得出来、还看不准。拒收判据（魔数/版本/长度自洽）同样在这里钉。
#
# tools/setpoint_send.py 是同一份格式的 Python 实现，改偏移表时两处一起改；
# checks/setpoint_channel_check.py 拿同一组用例对两边。
add_executable(test_setpoint_wire unit/setpoint/test_setpoint_wire.c)
target_link_libraries(test_setpoint_wire PRIVATE emaster::setpoint_wire)
emaster_enable_strict_warnings(test_setpoint_wire)
add_test(NAME setpoint_wire COMMAND test_setpoint_wire)

# PDO 条目判定：设备实际映射与厂商 ESI 声明只差一个假字节（MT 台架实测的形态）。
#
# 这条要紧在于判错了不报错，只是安静地放行：主站会以为"选对了映射表"等于"表里内容也对"，
# 而实际映射到过程映像里的字节来自哪张表没人核对。判据不依赖 SOEM，开发机上能跑。
add_executable(test_pdo_layout_match unit/protocol/test_pdo_layout_match.c)
target_link_libraries(test_pdo_layout_match PRIVATE emaster::session emaster::catalog
                                                      emaster::config)
emaster_enable_strict_warnings(test_pdo_layout_match)
add_test(NAME pdo_layout_match COMMAND test_pdo_layout_match)

# 同一台设备上的后一半：判定说"按设备实际建映像"之后，映像得真的建得起来、编得出去、
# 解得回来。上面那条只管判定，这条管判定之后的编解码 —— 两边对"填充"的说法不一致时，
# 判定放行了，编解码照样会把一台能用的设备拒在 OP 门外。
add_executable(test_adopted_layout_codec unit/cia402/test_adopted_layout_codec.c)
target_link_libraries(test_adopted_layout_codec PRIVATE emaster::cia402 emaster::catalog
                                                       emaster::session)
target_include_directories(test_adopted_layout_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
emaster_enable_strict_warnings(test_adopted_layout_codec)
add_test(NAME adopted_layout_codec COMMAND test_adopted_layout_codec)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 报告写出这条路两个用例，共用 unit/audit/report_fixture.h 里的同一份夹具：
    # 一份问"字段的值对不对"，一份问"归档削旧既删了又出声吗"。共用夹具是要紧的
    # ——两边各造一份，迟早问的不是同一份报告。
    #
    # 只在 Linux 上建，与 src/audit 的 report 库同因（那个库本身就这么门控着）。
    # 报告主体份数/日志那半边的判别力由 checks/report_archive_check.py
    # 用变异对照另外看住：删除留声是子进程 stderr 的事，在 C 里判得重定向再恢复 fd，
    # 反而更容易把要验的东西写歪。
    add_executable(test_report_fault_fields unit/audit/test_report_fault_fields.c)
    target_link_libraries(test_report_fault_fields PRIVATE emaster::run_report emaster::cyclic)
    target_include_directories(test_report_fault_fields PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit/audit)
    emaster_enable_strict_warnings(test_report_fault_fields)
    add_test(NAME report_fault_fields
             COMMAND test_report_fault_fields ${CMAKE_CURRENT_BINARY_DIR}/report-fault-fixture.json)

    add_executable(test_report_prune unit/audit/test_report_prune.c)
    target_link_libraries(test_report_prune PRIVATE emaster::run_report emaster::cyclic)
    target_include_directories(test_report_prune PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit/audit)
    emaster_enable_strict_warnings(test_report_prune)
    add_test(NAME report_prune COMMAND test_report_prune)
endif()
