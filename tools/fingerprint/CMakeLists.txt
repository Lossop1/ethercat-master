add_library(emaster_fingerprint_format STATIC
    fingerprint_json.c
    fingerprint_plan.c)
add_library(emaster::fingerprint_format ALIAS emaster_fingerprint_format)

target_link_libraries(emaster_fingerprint_format PRIVATE emaster::catalog)
target_compile_features(emaster_fingerprint_format PUBLIC c_std_11)
target_include_directories(
    emaster_fingerprint_format
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>")
emaster_enable_strict_warnings(emaster_fingerprint_format)

if(EMASTER_BUILD_HARDWARE_TOOLS)
    add_executable(emaster-fingerprint main.c)
    target_link_libraries(
        emaster-fingerprint
        PRIVATE emaster::soem_adapter emaster::fingerprint_format)
    target_compile_features(emaster-fingerprint PRIVATE c_std_11)
    emaster_enable_strict_warnings(emaster-fingerprint)
endif()
