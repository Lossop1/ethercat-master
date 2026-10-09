add_executable(emaster-status status_client.c)

target_include_directories(emaster-status PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(emaster-status PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-status)
