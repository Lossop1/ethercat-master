add_library(emaster_cia402 STATIC controller.c process_image.c)
add_library(emaster::cia402 ALIAS emaster_cia402)

# process_image.c 从 src/bus/soem 移进来时带着它的依赖：PDO 编解码与布局来自
# emaster::protocol/emaster::pdo_codec，PDO 审计来自 emaster::run_audit。
# 原先这些由 emaster_soem_control_session 提供，移进来后必须自己声明。
target_link_libraries(emaster_cia402 PUBLIC emaster::protocol emaster::pdo_codec
                                            emaster::run_audit)

target_compile_features(emaster_cia402 PUBLIC c_std_11)
target_include_directories(
    emaster_cia402
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_cia402)
