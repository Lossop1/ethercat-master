cmake_minimum_required(VERSION 3.28)

project(
    emaster
    VERSION 0.1.0
    DESCRIPTION "Safety-gated EtherCAT master for a 12-axis robot"
    LANGUAGES C)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_emaster_hardware_default ON)
else()
    set(_emaster_hardware_default OFF)
endif()

option(EMASTER_BUILD_HARDWARE_TOOLS
       "Build Linux/SOEM diagnostic tools that can access an EtherCAT network"
       ${_emaster_hardware_default})

set(EMASTER_CATALOG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/config/slaves/cyberbeast_isvd90rc_300b_100_70.json")
set(EMASTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EMASTER_GENERATED_CATALOG "${EMASTER_GENERATED_DIR}/slave_catalog.c")

add_custom_command(
    OUTPUT "${EMASTER_GENERATED_CATALOG}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_slave_catalog.py"
        --input "${EMASTER_CATALOG_FILE}"
        --output "${EMASTER_GENERATED_CATALOG}"
    DEPENDS
        "${EMASTER_CATALOG_FILE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_slave_catalog.py"
    COMMENT "Generating the compiled slave catalog"
    VERBATIM)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emaster/version.h.in"
    "${EMASTER_GENERATED_DIR}/emaster/version.h"
    @ONLY)

add_library(emaster_core STATIC
    src/core/slave_profile.c
    "${EMASTER_GENERATED_CATALOG}")
add_library(emaster::core ALIAS emaster_core)

target_compile_features(emaster_core PUBLIC c_std_11)
target_include_directories(
    emaster_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${EMASTER_GENERATED_DIR}>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_core)

if(EMASTER_BUILD_HARDWARE_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Hardware diagnostic tools are supported only on Linux")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/SOEM/CMakeLists.txt")
        message(FATAL_ERROR
            "SOEM submodule is missing. Run: git submodule update --init --recursive")
    endif()

    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "Do not build upstream SOEM samples" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build SOEM as a static library" FORCE)
    add_subdirectory(external/SOEM EXCLUDE_FROM_ALL)
    add_subdirectory(src/soem)
    add_subdirectory(apps/fingerprint)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
