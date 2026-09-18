# 观测通道。刻意不依赖 SOEM，也不依赖任何其它工程库——整条通道除了"周期线程发布
# 那一行调用点"以外，都能在开发机和 CI 上无 EtherCAT 硬件地验证。这是把库拆出来
# 的主要理由，不是分层洁癖。

# slow.c 单独一个目标，因为它**只依赖 libc**：不碰 pthread，也不碰 socket。
# 这样"报告里该印读数还是印「—」"这条判定就能在只有 MinGW 的开发机上被单元测试
# 钉住（emaster_observation 里的 server.c 要 poll.h，Windows 上编不过）。
# 改动这条判定要能立刻看到红灯，不能只在台架停机报告里看。
add_library(emaster_observation_slow STATIC slow.c)
add_library(emaster::observation_slow ALIAS emaster_observation_slow)

target_compile_features(emaster_observation_slow PUBLIC c_std_11)
target_include_directories(
    emaster_observation_slow
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_observation_slow)

add_library(emaster_observation STATIC
    frame.c
    ring.c
    wire.c
    server.c)
add_library(emaster::observation ALIAS emaster_observation)

# server.c 用 pthread 起监听线程。仍然不依赖 SOEM。
find_package(Threads REQUIRED)
target_link_libraries(emaster_observation PUBLIC Threads::Threads emaster_observation_slow)

target_compile_features(emaster_observation PUBLIC c_std_11)
target_include_directories(
    emaster_observation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_observation)
