add_executable(emaster-dc-prepare main.c)
target_link_libraries(
    emaster-dc-prepare
    PRIVATE emaster::soem_dc_prepare emaster::session emaster::config emaster::messages)
target_compile_features(emaster-dc-prepare PRIVATE c_std_11)
target_include_directories(emaster-dc-prepare PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
emaster_enable_strict_warnings(emaster-dc-prepare)
