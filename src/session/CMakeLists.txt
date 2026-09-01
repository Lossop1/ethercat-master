add_library(emaster_session STATIC session_plan.c)
add_library(emaster::session ALIAS emaster_session)

target_compile_features(emaster_session PUBLIC c_std_11)
target_include_directories(
    emaster_session
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
target_link_libraries(emaster_session PUBLIC emaster::catalog emaster::config)
emaster_enable_strict_warnings(emaster_session)
