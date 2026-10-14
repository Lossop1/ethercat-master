add_library(emaster_run_audit STATIC run_audit.c)
add_library(emaster::run_audit ALIAS emaster_run_audit)

target_compile_features(emaster_run_audit PUBLIC c_std_11)
target_include_directories(
    emaster_run_audit
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_run_audit)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(
        emaster_run_report
        STATIC
            run_report.c
            report_error_class.c
            report_sections.c
            json_writer.c)
    add_library(emaster::run_report ALIAS emaster_run_report)

    # setpoint_wire 只为一句拒收原因的文本：报告里那一列必须与主站往 stderr 打的
    # 是同一句话。它只依赖 libc，不把套接字拖进审计层。
    target_link_libraries(emaster_run_report PUBLIC emaster::diagnostics emaster::setpoint_wire)

    target_compile_features(emaster_run_report PUBLIC c_std_11)
    target_include_directories(
        emaster_run_report
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>")
    emaster_enable_strict_warnings(emaster_run_report)
endif()
