add_executable(emaster-fingerprint main.c)
target_link_libraries(emaster-fingerprint PRIVATE emaster::soem_adapter)
target_compile_features(emaster-fingerprint PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-fingerprint)
