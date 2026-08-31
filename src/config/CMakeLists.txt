add_custom_command(
    OUTPUT "${EMASTER_GENERATED_CONFIG}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_runtime_config.py"
        --topology-input ${EMASTER_TOPOLOGY_CONFIG_FILES}
        --deployment-input ${EMASTER_DEPLOYMENT_CONFIG_FILES}
        --output "${EMASTER_GENERATED_CONFIG}"
    DEPENDS
        ${EMASTER_TOPOLOGY_CONFIG_FILES}
        ${EMASTER_DEPLOYMENT_CONFIG_FILES}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_runtime_config.py"
    COMMENT "生成编译期拓扑和部署目录"
    VERBATIM)

add_library(emaster_config STATIC "${EMASTER_GENERATED_CONFIG}")
add_library(emaster::config ALIAS emaster_config)

target_compile_features(emaster_config PUBLIC c_std_11)
target_include_directories(
    emaster_config
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<BUILD_INTERFACE:${EMASTER_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_config)
