# 诊断分类：把从站/驱动器报回来的数字变成有名字的类别。
#
# 刻意做成**叶子目标**：不链 SOEM、不链会话、连 pthread 都不依赖。理由是它要把
# "31 个控制状态 + 逐轴 AL 证据 -> 哪一类事故"这条判据做成能在开发机上单测的纯逻辑；
# 一旦拖进总线或会话头文件，Windows 侧就建不起来了（那正是 src/cia402 的反例：
# 那个目标 PUBLIC 链了 protocol/pdo_codec/run_audit，纯逻辑测试没法只链它）。
add_library(emaster_diagnostics STATIC
    al_status.c
    cia402_error.c
    error_class.c)
add_library(emaster::diagnostics ALIAS emaster_diagnostics)

target_compile_features(emaster_diagnostics PUBLIC c_std_11)
target_include_directories(
    emaster_diagnostics
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_diagnostics)
