cmake_minimum_required(VERSION 3.28)

project(
    emaster
    VERSION 0.1.0
    DESCRIPTION "具有安全阶段门的 12 轴机器人 EtherCAT 主站"
    LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(EMASTER_MESSAGE_LOCALE "zh_CN" CACHE STRING "编译时使用的消息资源语言标识")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_emaster_hardware_default ON)
else()
    set(_emaster_hardware_default OFF)
endif()

option(EMASTER_BUILD_HARDWARE_TOOLS
       "构建可访问 EtherCAT 网络的 Linux/SOEM 诊断工具"
       ${_emaster_hardware_default})

# 设备配置是可扩展的输入集合。CONFIGURE_DEPENDS 使新增或删除型号后 CMake 自动重新配置，
# 避免在构建脚本中维护单个设备文件名。
file(GLOB EMASTER_DEVICE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/devices/*.json")
if(NOT EMASTER_DEVICE_CONFIG_FILES)
    message(FATAL_ERROR "config/devices 中至少需要一个设备配置")
endif()

# 拓扑和部署也是生成目录的输入集合。这里只发现文件，不在 CMake 中解释具体拓扑、网卡或数量。
file(GLOB EMASTER_TOPOLOGY_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/topologies/*.json")
file(GLOB EMASTER_DEPLOYMENT_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/deployments/*.json")
file(GLOB EMASTER_OPERATION_PROFILE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/operation_profiles/*.json")
if(NOT EMASTER_TOPOLOGY_CONFIG_FILES OR NOT EMASTER_DEPLOYMENT_CONFIG_FILES OR
   NOT EMASTER_OPERATION_PROFILE_CONFIG_FILES)
    message(FATAL_ERROR
        "config/topologies、config/deployments 和 config/operation_profiles 中都至少需要一个配置")
endif()

set(EMASTER_MESSAGE_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/config/messages/${EMASTER_MESSAGE_LOCALE}.json")
if(NOT EXISTS "${EMASTER_MESSAGE_CONFIG_FILE}")
    message(FATAL_ERROR "找不到消息资源：${EMASTER_MESSAGE_CONFIG_FILE}")
endif()

set(EMASTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EMASTER_GENERATED_CATALOG "${EMASTER_GENERATED_DIR}/slave_catalog.c")
set(EMASTER_GENERATED_CONFIG "${EMASTER_GENERATED_DIR}/runtime_config.c")
set(EMASTER_GENERATED_MESSAGES "${EMASTER_GENERATED_DIR}/messages.c")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emaster/version.h.in"
    "${EMASTER_GENERATED_DIR}/emaster/version.h"
    @ONLY)

add_subdirectory(src/protocol)
add_subdirectory(src/catalog)
add_subdirectory(src/config)
add_subdirectory(src/messages)

if(EMASTER_BUILD_HARDWARE_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "硬件诊断工具仅支持 Linux")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/SOEM/CMakeLists.txt")
        message(FATAL_ERROR
            "缺少 SOEM 子模块，请运行：git submodule update --init --recursive")
    endif()

    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "不构建上游 SOEM 示例" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "将 SOEM 构建为静态库" FORCE)
    add_subdirectory(external/SOEM EXCLUDE_FROM_ALL)
    add_subdirectory(src/bus/soem)
endif()

add_subdirectory(tools/fingerprint)
