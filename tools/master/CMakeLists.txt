add_executable(emaster-master main.c)

target_link_libraries(
    emaster-master
    PRIVATE emaster::soem_control_session emaster::session emaster::config emaster::messages)
target_compile_features(emaster-master PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-master)
