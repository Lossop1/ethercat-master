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
        ${CMAKE_SOURCE_DIR}/src/bus/soem/cycle_clock.c)
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

# 慢速遥测"读到过没有、多久以前读到的"：报告里那几个 0 该印成读数还是印「—」全靠
# 这一条判定，而判错了不会有任何报错，只会安静地把"没读到"写成温度 0（P11.8）。
# 只依赖 slow.c（纯 libc），所以开发机上也能建、能跑。
add_executable(test_slow_age unit/observation/test_slow_age.c)
target_link_libraries(test_slow_age PRIVATE emaster::observation_slow)
emaster_enable_strict_warnings(test_slow_age)
add_test(NAME slow_age COMMAND test_slow_age)
