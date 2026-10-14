add_custom_command(
    OUTPUT "${EMASTER_GENERATED_ERROR_RECOVERY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen/generate_error_recovery_config.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../config/error_recovery_policies"
        "${EMASTER_GENERATED_ERROR_RECOVERY}"
    DEPENDS
        ${EMASTER_ERROR_RECOVERY_POLICY_FILES}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen/generate_error_recovery_config.py"
    COMMENT "生成错误恢复策略配置"
    VERBATIM)

# 运行时加载版本：配置不再编进二进制。
#
# 生成物（三个 .c）由根 CMakeLists 的 add_custom_command 在构建时从 config/*.json
# 产出，落在构建目录；主站在启动时从 config-runtime/ 读扁平文件填进同一批结构体。
# 访问函数签名一个都没变，所以上层几百处调用不用改。
add_library(emaster_config STATIC
    config_reader.c
    runtime_config_load.c
    "${EMASTER_GENERATED_DIR}/topologies.c"
    "${EMASTER_GENERATED_DIR}/runtime.c"
    "${EMASTER_GENERATED_DIR}/deployments.c"
    "${EMASTER_GENERATED_ERROR_RECOVERY}")
add_library(emaster::config ALIAS emaster_config)
add_dependencies(emaster_config emaster_generated_config)

target_compile_features(emaster_config PUBLIC c_std_11)
target_include_directories(
    emaster_config
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<BUILD_INTERFACE:${EMASTER_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_config)
