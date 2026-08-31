add_executable(emaster_catalog_tests test_slave_profile.c)
target_link_libraries(emaster_catalog_tests PRIVATE emaster::catalog)
target_compile_features(emaster_catalog_tests PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster_catalog_tests)

add_test(NAME unit.catalog.slave_profile COMMAND emaster_catalog_tests)

# 生成器测试直接验证多设备输入和失效关闭行为，不需要编译临时 C 工程。
add_test(
    NAME unit.catalog.generator
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_generate_slave_catalog.py")
