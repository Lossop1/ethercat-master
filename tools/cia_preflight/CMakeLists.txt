add_executable(emaster-cia-preflight main.c)

target_link_libraries(
    emaster-cia-preflight
    PRIVATE emaster::soem_cia_preflight emaster::session emaster::config emaster::messages)
target_compile_features(emaster-cia-preflight PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-cia-preflight)
