add_library(emaster_protocol STATIC pdo_layout.c)
add_library(emaster::protocol ALIAS emaster_protocol)

target_compile_features(emaster_protocol PUBLIC c_std_11)
target_include_directories(
    emaster_protocol
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_protocol)

add_library(emaster_pdo_codec STATIC pdo_codec.c)
add_library(emaster::pdo_codec ALIAS emaster_pdo_codec)

target_compile_features(emaster_pdo_codec PUBLIC c_std_11)
target_include_directories(
    emaster_pdo_codec
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
target_link_libraries(emaster_pdo_codec PUBLIC emaster::protocol)
emaster_enable_strict_warnings(emaster_pdo_codec)
