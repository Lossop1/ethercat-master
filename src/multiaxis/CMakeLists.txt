add_library(emaster_multiaxis STATIC coordinator.c)
add_library(emaster::multiaxis ALIAS emaster_multiaxis)

target_compile_features(emaster_multiaxis PUBLIC c_std_11)
target_include_directories(
    emaster_multiaxis
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
target_link_libraries(emaster_multiaxis PUBLIC emaster::cia402)
emaster_enable_strict_warnings(emaster_multiaxis)
