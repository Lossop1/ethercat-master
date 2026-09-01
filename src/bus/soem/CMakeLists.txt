add_library(emaster_soem_adapter STATIC preop_probe.c)
add_library(emaster::soem_adapter ALIAS emaster_soem_adapter)

target_link_libraries(emaster_soem_adapter PRIVATE soem emaster::protocol)
target_compile_features(emaster_soem_adapter PUBLIC c_std_11)
target_include_directories(
    emaster_soem_adapter
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>")
emaster_enable_strict_warnings(emaster_soem_adapter)
