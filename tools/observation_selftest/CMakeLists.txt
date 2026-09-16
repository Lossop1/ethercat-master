# 离线自测不进 install：它是验证手段，不是交付物。
# 只依赖 emaster::observation 与线程库，因此 CI 与开发机都能跑，不需要 EtherCAT 硬件。

find_package(Threads REQUIRED)

add_executable(observation-selftest main.c)
target_link_libraries(observation-selftest PRIVATE emaster::observation Threads::Threads)
target_compile_features(observation-selftest PRIVATE c_std_11)
emaster_enable_strict_warnings(observation-selftest)
