add_library(emaster_session STATIC session_plan.c plan_failure.c)
add_library(emaster::session ALIAS emaster_session)

target_compile_features(emaster_session PUBLIC c_std_11)
target_include_directories(
    emaster_session
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
# messages 只为了让中止原因三个入口说同一句话（见 plan_failure.c），不参与判定。
target_link_libraries(
    emaster_session
    PUBLIC emaster::catalog emaster::config
    PRIVATE emaster::messages)
emaster_enable_strict_warnings(emaster_session)
