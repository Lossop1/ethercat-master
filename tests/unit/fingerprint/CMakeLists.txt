add_executable(emaster_fingerprint_format_tests test_fingerprint_json.c)
target_link_libraries(emaster_fingerprint_format_tests PRIVATE emaster::fingerprint_format)
target_compile_features(emaster_fingerprint_format_tests PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster_fingerprint_format_tests)

add_test(
    NAME unit.fingerprint.json
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_fingerprint_json.py"
        --executable "$<TARGET_FILE:emaster_fingerprint_format_tests>")
