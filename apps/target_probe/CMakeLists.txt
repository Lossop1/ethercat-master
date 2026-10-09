add_executable(emaster-target-probe main.c)

target_link_libraries(
    emaster-target-probe
    PRIVATE emaster::soem_control_session emaster::run_report emaster::session
            emaster::config emaster::messages emaster::motion)
target_compile_features(emaster-target-probe PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-target-probe)
