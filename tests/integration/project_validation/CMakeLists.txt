add_test(
    NAME integration.project_validation
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/validate_project.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# 参数化夹具覆盖不同拓扑规模和跨层引用错误，防止具体台架数据渗入通用逻辑。
add_test(
    NAME integration.project_validation.contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_validate_project.py")
