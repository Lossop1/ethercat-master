# 观测通道。刻意不依赖 SOEM，也不依赖任何其它工程库——整条通道除了"周期线程发布
# 那一行调用点"以外，都能在开发机和 CI 上无 EtherCAT 硬件地验证。这是把库拆出来
# 的主要理由，不是分层洁癖。

add_library(emaster_observation STATIC
    frame.c
    ring.c
    slow.c
    wire.c
    server.c)
add_library(emaster::observation ALIAS emaster_observation)

# server.c 用 pthread 起监听线程。仍然不依赖 SOEM。
find_package(Threads REQUIRED)
target_link_libraries(emaster_observation PUBLIC Threads::Threads)

target_compile_features(emaster_observation PUBLIC c_std_11)
target_include_directories(
    emaster_observation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_observation)
