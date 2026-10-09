add_library(emaster_catalog STATIC
    slave_profile.c
    "${EMASTER_GENERATED_DIR}/catalog.c")
add_library(emaster::catalog ALIAS emaster_catalog)
add_dependencies(emaster_catalog emaster_generated_config)

target_compile_features(emaster_catalog PUBLIC c_std_11)
target_link_libraries(emaster_catalog PUBLIC emaster::protocol)
target_include_directories(
    emaster_catalog
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<BUILD_INTERFACE:${EMASTER_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_catalog)
