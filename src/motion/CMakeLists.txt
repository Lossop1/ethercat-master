add_library(emaster_motion STATIC relative_position.c velocity_profile.c position_target.c)
add_library(emaster::motion ALIAS emaster_motion)

target_compile_features(emaster_motion PUBLIC c_std_11)
target_include_directories(
    emaster_motion
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
target_link_libraries(emaster_motion PUBLIC emaster::config)
emaster_enable_strict_warnings(emaster_motion)
