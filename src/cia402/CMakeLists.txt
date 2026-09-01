add_library(emaster_cia402 STATIC controller.c)
add_library(emaster::cia402 ALIAS emaster_cia402)

target_compile_features(emaster_cia402 PUBLIC c_std_11)
target_include_directories(
    emaster_cia402
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_cia402)
