# 设点通道。与观测通道同一取舍：刻意不依赖 SOEM，只认一份外部目标缓冲区。
# wire.c 只依赖 libc，因此记录格式的往返与拒收判据能在开发机上被单元测试钉住
# （server.c 要 poll.h / sys/socket.h，Windows 上编不过）。

add_library(emaster_setpoint_wire STATIC wire.c)
add_library(emaster::setpoint_wire ALIAS emaster_setpoint_wire)

target_compile_features(emaster_setpoint_wire PUBLIC c_std_11)
target_include_directories(
    emaster_setpoint_wire
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_setpoint_wire)

add_library(emaster_setpoint STATIC server.c)
add_library(emaster::setpoint ALIAS emaster_setpoint)

# server.c 用 pthread 起监听线程。emaster_thread_leave_realtime 是 thread_sched.h 里的
# static inline，不引库。
find_package(Threads REQUIRED)
target_link_libraries(
    emaster_setpoint
    PUBLIC Threads::Threads emaster_setpoint_wire)

target_compile_features(emaster_setpoint PUBLIC c_std_11)
target_include_directories(
    emaster_setpoint
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_setpoint)
