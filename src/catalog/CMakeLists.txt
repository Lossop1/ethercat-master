add_custom_command(
    OUTPUT "${EMASTER_GENERATED_CATALOG}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_slave_catalog.py"
        --input ${EMASTER_DEVICE_CONFIG_FILES}
        --output "${EMASTER_GENERATED_CATALOG}"
    DEPENDS
        ${EMASTER_DEVICE_CONFIG_FILES}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_slave_catalog.py"
    COMMENT "生成编译期从站目录"
    VERBATIM)

add_library(emaster_catalog STATIC
    slave_profile.c
    "${EMASTER_GENERATED_CATALOG}")
add_library(emaster::catalog ALIAS emaster_catalog)

target_compile_features(emaster_catalog PUBLIC c_std_11)
target_link_libraries(emaster_catalog PUBLIC emaster::protocol)
target_include_directories(
    emaster_catalog
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<BUILD_INTERFACE:${EMASTER_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_catalog)
