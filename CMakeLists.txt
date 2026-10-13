cmake_minimum_required(VERSION 3.28)

project(
    emaster
    VERSION 0.1.0
    DESCRIPTION "具有安全阶段门的多轴机器人 EtherCAT 主站"
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

# 离线单元测试：不依赖 SOEM、不访问总线，任何平台都能建、都能跑。
option(EMASTER_BUILD_TESTS "构建离线单元测试" ON)

# 设备配置是可扩展的输入集合。CONFIGURE_DEPENDS 使新增或删除型号后 CMake 自动重新配置，
# 避免在构建脚本中维护单个设备文件名。
#
# 空目录是合法状态，不是错误：这个项目交付时不含任何电机配置，客户用自己的 ESI 跑
# gen/emaster_config.py 生成。构建必须先于配置成立 —— 生成出来的 C 与配置内容无关
# （条目数在运行期从扁平文件读），所以二进制建得出来，只是启动时说"没有部署"。
file(GLOB EMASTER_DEVICE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/devices/*.json")

# 拓扑和部署也是生成目录的输入集合。这里只发现文件，不在 CMake 中解释具体拓扑、网卡或数量。
file(GLOB EMASTER_TOPOLOGY_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/topologies/*.json")
file(GLOB EMASTER_DEPLOYMENT_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/deployments/*.json")
file(GLOB EMASTER_OPERATION_PROFILE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/operation_profiles/*.json")
file(GLOB EMASTER_MOTION_PROFILE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/motion_profiles/*.json")
file(GLOB EMASTER_ERROR_RECOVERY_POLICY_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/error_recovery_policies/*.json")
if(NOT EMASTER_TOPOLOGY_CONFIG_FILES OR NOT EMASTER_DEPLOYMENT_CONFIG_FILES OR
   NOT EMASTER_OPERATION_PROFILE_CONFIG_FILES)
    message(STATUS "拓扑/部署/运行方案有空目录：按空配置生成，主站启动时会找不到部署")
endif()
if(NOT EMASTER_ERROR_RECOVERY_POLICY_FILES)
    message(WARNING "config/error_recovery_policies 中未找到错误恢复策略，将使用空配置")
endif()

set(EMASTER_MESSAGE_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/config/messages/${EMASTER_MESSAGE_LOCALE}.json")
if(NOT EXISTS "${EMASTER_MESSAGE_CONFIG_FILE}")
    message(FATAL_ERROR "找不到消息资源：${EMASTER_MESSAGE_CONFIG_FILE}")
endif()

set(EMASTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
# ---------------------------------------------------------------------------
# 配置的生成物。四个生成器各管一族，顺序不能颠倒：部署按 ID 引用前三者。
#
# 为什么不把配置直接编进二进制（原来的做法）：
#   那样主机名、网卡名、轴数、电机型号都成了编译期常量，换任何一样都要重编译，
#   而第三方拿到二进制之后改不了任何东西。现在的做法是：生成器产出"填充代码"，
#   主站在启动时从 config-runtime/ 读扁平文件填进同一批结构体。公开访问函数的
#   签名一个都没变，所以 src/ 里的调用不用改。
#
# 为什么要在这里重新生成、而不是把产物签入就算了：
#   签入的话，改了 config/*.json 却忘了跑生成器，产物就静默陈旧 —— 而"静默陈旧"
#   正是这次改造要消灭的东西。挂到构建上，改配置后重新构建即可。
#
# .c/.h 落在构建目录，保持出源码构建干净；.flat 必须落在源码树的 config-runtime/，
# 因为主站的默认配置目录就是它（相对当前工作目录），台架脚本据此裸启动主站。
# 生成是确定性的：同样的输入产出逐字节相同的文件，所以不会每次构建都弄脏源码树。
# ---------------------------------------------------------------------------
set(EMASTER_RUNTIME_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime")
set(EMASTER_GENERATED_HEADER_DIR "${EMASTER_GENERATED_DIR}/emaster/config")

add_custom_command(
    OUTPUT "${EMASTER_GENERATED_DIR}/topologies.c"
           "${EMASTER_GENERATED_DIR}/emaster/config/generated_apply.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/topologies.flat"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}/emaster/config"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_config_apply.py"
            --topologies "${CMAKE_CURRENT_SOURCE_DIR}/config/topologies"
            --c-output "${EMASTER_GENERATED_DIR}/topologies.c"
            --h-output "${EMASTER_GENERATED_DIR}/emaster/config/generated_apply.h"
            --flat-output "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/topologies.flat"
    DEPENDS ${EMASTER_TOPOLOGY_CONFIG_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_config_apply.py"
    COMMENT "生成拓扑族的填充代码与运行时配置"
    VERBATIM)

add_custom_command(
    OUTPUT "${EMASTER_GENERATED_DIR}/catalog.c"
           "${EMASTER_GENERATED_DIR}/emaster/config/generated_catalog.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/catalog.flat"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_catalog_apply.py"
            --devices "${CMAKE_CURRENT_SOURCE_DIR}/config/devices"
            --c-output "${EMASTER_GENERATED_DIR}/catalog.c"
            --h-output "${EMASTER_GENERATED_DIR}/emaster/config/generated_catalog.h"
            --flat-output "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/catalog.flat"
    DEPENDS ${EMASTER_DEVICE_CONFIG_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_catalog_apply.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_slave_catalog.py"
    COMMENT "生成设备目录族的填充代码与运行时配置"
    VERBATIM)

add_custom_command(
    OUTPUT "${EMASTER_GENERATED_DIR}/runtime.c"
           "${EMASTER_GENERATED_DIR}/emaster/config/generated_runtime.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/runtime.flat"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_runtime_apply.py"
            --operations "${CMAKE_CURRENT_SOURCE_DIR}/config/operation_profiles"
            --motions "${CMAKE_CURRENT_SOURCE_DIR}/config/motion_profiles"
            --c-output "${EMASTER_GENERATED_DIR}/runtime.c"
            --h-output "${EMASTER_GENERATED_DIR}/emaster/config/generated_runtime.h"
            --flat-output "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/runtime.flat"
    DEPENDS ${EMASTER_OPERATION_PROFILE_CONFIG_FILES} ${EMASTER_MOTION_PROFILE_CONFIG_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_runtime_apply.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/runtime_config_model.py"
    COMMENT "生成运行方案与运动方案族的填充代码与运行时配置"
    VERBATIM)

add_custom_command(
    OUTPUT "${EMASTER_GENERATED_DIR}/deployments.c"
           "${EMASTER_GENERATED_DIR}/emaster/config/generated_deployment.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/deployments.flat"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_deployment_apply.py"
            --deployments "${CMAKE_CURRENT_SOURCE_DIR}/config/deployments"
            --topologies "${CMAKE_CURRENT_SOURCE_DIR}/config/topologies"
            --operations "${CMAKE_CURRENT_SOURCE_DIR}/config/operation_profiles"
            --motions "${CMAKE_CURRENT_SOURCE_DIR}/config/motion_profiles"
            --c-output "${EMASTER_GENERATED_DIR}/deployments.c"
            --h-output "${EMASTER_GENERATED_DIR}/emaster/config/generated_deployment.h"
            --flat-output "${CMAKE_CURRENT_SOURCE_DIR}/config-runtime/deployments.flat"
    DEPENDS ${EMASTER_DEPLOYMENT_CONFIG_FILES} ${EMASTER_TOPOLOGY_CONFIG_FILES}
            ${EMASTER_OPERATION_PROFILE_CONFIG_FILES} ${EMASTER_MOTION_PROFILE_CONFIG_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/generate_deployment_apply.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/runtime_config_model.py"
    COMMENT "生成部署族的填充代码与运行时配置"
    VERBATIM)

# 聚合目标：两个库都要依赖它。
#
# 为什么必须显式依赖：源码里 include 了生成的头文件（generated_apply.h 等），
# 而"源文件依赖某个头文件"这件事 Ninja 在配置期还不知道 —— 它只知道生成的 .c
# 是库的源文件。于是并行构建时会先编 runtime_config_load.c，而那时头文件还没写
# 出来，报的是"没有那个文件"。用 add_dependencies 把顺序钉死。
add_custom_target(emaster_generated_config ALL
    DEPENDS
        "${EMASTER_GENERATED_DIR}/topologies.c"
        "${EMASTER_GENERATED_DIR}/catalog.c"
        "${EMASTER_GENERATED_DIR}/runtime.c"
        "${EMASTER_GENERATED_DIR}/deployments.c"
        "${EMASTER_GENERATED_DIR}/emaster/config/generated_apply.h"
        "${EMASTER_GENERATED_DIR}/emaster/config/generated_catalog.h"
        "${EMASTER_GENERATED_DIR}/emaster/config/generated_runtime.h"
        "${EMASTER_GENERATED_DIR}/emaster/config/generated_deployment.h")
set(EMASTER_GENERATED_CATALOG "${EMASTER_GENERATED_DIR}/slave_catalog.c")
set(EMASTER_GENERATED_CONFIG "${EMASTER_GENERATED_DIR}/runtime_config.c")
set(EMASTER_GENERATED_MESSAGES "${EMASTER_GENERATED_DIR}/messages.c")
set(EMASTER_GENERATED_ERROR_RECOVERY "${EMASTER_GENERATED_DIR}/error_recovery_config.c")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emaster/version.h.in"
    "${EMASTER_GENERATED_DIR}/emaster/version.h"
    @ONLY)

add_subdirectory(src/protocol)
add_subdirectory(src/diagnostics)
add_subdirectory(src/catalog)
add_subdirectory(src/config)
add_subdirectory(src/messages)
add_subdirectory(src/audit)
add_subdirectory(src/session)
add_subdirectory(src/motion)
add_subdirectory(src/cia402)
add_subdirectory(src/multiaxis)
add_subdirectory(src/cyclic)
add_subdirectory(src/safety)

if(EMASTER_BUILD_HARDWARE_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "硬件诊断工具仅支持 Linux")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/SOEM/CMakeLists.txt")
        message(FATAL_ERROR
            "external/SOEM 缺失。交付树自带 SOEM 源码，请重新获取交付件")
    endif()

    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "不构建上游 SOEM 示例" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "将 SOEM 构建为静态库" FORCE)
    add_subdirectory(external/SOEM EXCLUDE_FROM_ALL)
    add_subdirectory(src/bus)
    add_subdirectory(apps/dc_prepare)
    add_subdirectory(apps/emaster_watch)
    add_subdirectory(apps/emaster_move)
    add_subdirectory(apps/emaster_move_deg)
    add_subdirectory(apps/master)
    add_subdirectory(apps/status_client)
    add_subdirectory(apps/target_probe)
    add_subdirectory(apps/diagnose_slave)
    add_subdirectory(apps/disable_drives)
endif()

add_subdirectory(src/observation)
add_subdirectory(apps/observation_selftest)
add_subdirectory(apps/fingerprint)

# 测试放在最后：它们链接上面的库，顺序反了就成了"目标不存在"。
if(EMASTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
