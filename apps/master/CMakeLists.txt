add_executable(emaster-master main.c console.c)

target_link_libraries(
    emaster-master
    PRIVATE emaster::soem_control_session emaster::run_report emaster::session
            emaster::config emaster::messages m pthread)
target_compile_features(emaster-master PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-master)
