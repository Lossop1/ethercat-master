add_library(emaster_soem_common STATIC soem_common.c)
add_library(emaster::soem_common ALIAS emaster_soem_common)

target_link_libraries(emaster_soem_common PRIVATE soem emaster::protocol)
target_compile_features(emaster_soem_common PUBLIC c_std_11)
target_include_directories(
    emaster_soem_common
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>")
emaster_enable_strict_warnings(emaster_soem_common)

add_library(emaster_soem_adapter STATIC preop_probe.c)
add_library(emaster::soem_adapter ALIAS emaster_soem_adapter)

add_library(emaster_soem_dc_prepare STATIC dc_prepare.c)
add_library(emaster::soem_dc_prepare ALIAS emaster_soem_dc_prepare)

target_link_libraries(emaster_soem_adapter PRIVATE soem emaster::protocol)
target_compile_features(emaster_soem_adapter PUBLIC c_std_11)
target_include_directories(
    emaster_soem_adapter
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>")
emaster_enable_strict_warnings(emaster_soem_adapter)

target_link_libraries(emaster_soem_dc_prepare PRIVATE emaster::soem_common soem
                                                    emaster::session emaster::protocol
                                                    emaster::catalog)
target_compile_features(emaster_soem_dc_prepare PUBLIC c_std_11)
target_include_directories(
    emaster_soem_dc_prepare
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>")
emaster_enable_strict_warnings(emaster_soem_dc_prepare)

add_library(emaster_soem_cia_preflight STATIC cia_preflight.c)
add_library(emaster::soem_cia_preflight ALIAS emaster_soem_cia_preflight)

target_link_libraries(emaster_soem_cia_preflight PRIVATE emaster::soem_common soem
                                                         emaster::session emaster::catalog
                                                         emaster::protocol emaster::pdo_codec)
target_compile_features(emaster_soem_cia_preflight PUBLIC c_std_11)
target_include_directories(
    emaster_soem_cia_preflight
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>")
emaster_enable_strict_warnings(emaster_soem_cia_preflight)
