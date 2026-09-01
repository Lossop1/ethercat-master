add_library(emaster_cyclic STATIC exchange.c)
add_library(emaster::cyclic ALIAS emaster_cyclic)

target_compile_features(emaster_cyclic PUBLIC c_std_11)
target_include_directories(
    emaster_cyclic
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_cyclic)
