add_executable(emaster_core_tests test_slave_profile.c)
target_link_libraries(emaster_core_tests PRIVATE emaster::core)
target_compile_features(emaster_core_tests PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster_core_tests)

add_test(NAME unit.slave_profile COMMAND emaster_core_tests)
add_test(
    NAME offline.project_validation
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_project.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}/..")
