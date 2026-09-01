add_library(emaster_safety STATIC gate.c)
add_library(emaster::safety ALIAS emaster_safety)

target_compile_features(emaster_safety PUBLIC c_std_11)
target_include_directories(
    emaster_safety
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_safety)
