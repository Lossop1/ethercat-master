# src/bus 是唯一接触 SOEM 与网卡的一层，因此整体在 EMASTER_BUILD_HARDWARE_TOOLS 门内。
# 内部按功能分四个子目录，各自是一个明确角色：
#
#   soem/     SOEM 适配：访问层、扫描探测、DC 配置、拓扑映射
#   session/  会话编排：建立、启动、周期、目标、监督、停机
#   observe/  观测生产：把主站内部状态发布到观测通道（通道本身在 src/observation）
#   io/       命令口：主机侧的 AF_UNIX 命令接口
#
# 这一层与 SOEM 是焊死的：会话直接持有 ecx_contextt。把它拆开需要一个不透明的
# 上下文句柄，那是重写而不是重排，所以这里只做归位。

# SOEM 访问层。soem_common.h 在本目录内共享，不属于公共主站接口。
add_library(emaster_soem_common STATIC soem/soem_common.c)
add_library(emaster::soem_common ALIAS emaster_soem_common)
target_link_libraries(emaster_soem_common PRIVATE soem emaster::protocol emaster::run_audit)
target_compile_features(emaster_soem_common PUBLIC c_std_11)
target_include_directories(
    emaster_soem_common
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/internal")
emaster_enable_strict_warnings(emaster_soem_common)

# 只读探测与拓扑映射：最高到 PRE-OP，不发过程数据。
add_library(emaster_soem_adapter STATIC soem/preop_probe.c soem/topology_mapper.c)
add_library(emaster::soem_adapter ALIAS emaster_soem_adapter)
target_link_libraries(emaster_soem_adapter PRIVATE soem emaster::protocol emaster::catalog)
target_compile_features(emaster_soem_adapter PUBLIC c_std_11)
target_include_directories(
    emaster_soem_adapter
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/internal")
emaster_enable_strict_warnings(emaster_soem_adapter)

# DC 配置：测传播延迟、写 SYNC0，并回读核对。
add_library(emaster_soem_dc_prepare STATIC soem/dc_prepare.c)
add_library(emaster::soem_dc_prepare ALIAS emaster_soem_dc_prepare)
target_link_libraries(emaster_soem_dc_prepare PRIVATE emaster::soem_common soem
                                                      emaster::session emaster::protocol
                                                      emaster::catalog)
target_compile_features(emaster_soem_dc_prepare PUBLIC c_std_11)
target_include_directories(
    emaster_soem_dc_prepare
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/internal")
emaster_enable_strict_warnings(emaster_soem_dc_prepare)

# 控制会话：会话编排 + 观测生产 + 命令口。
# session_internal.h 与 session_observer.h 要引 cia402/process_image.h 与 cyclic/clock.h，
# 两者都在 src/ 下但不是公共头，所以把 src/ 加进私有包含路径。
add_library(emaster_soem_control_session STATIC
    session/control_session.c session/session_setup.c session/session_start.c
    session/session_exchange.c session/session_control.c session/session_target.c
    session/session_supervisor.c session/session_shutdown.c
    observe/session_observer.c observe/session_observer_thread.c observe/session_observation.c
    io/command_server.c)
add_library(emaster::soem_control_session ALIAS emaster_soem_control_session)
target_link_libraries(emaster_soem_control_session PRIVATE emaster::soem_common soem
                                                         emaster::session emaster::catalog
                                                         emaster::protocol emaster::pdo_codec
                                                         emaster::multiaxis emaster::motion
                                                         emaster::run_audit emaster::cyclic
                                                         emaster::safety emaster::observation)
target_compile_features(emaster_soem_control_session PUBLIC c_std_11)
target_include_directories(
    emaster_soem_control_session
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    PRIVATE "${CMAKE_SOURCE_DIR}/src"
            # soem_common.h / session_internal.h / session_observer.h 这三个私有头跨
            # soem/、session/、observe/ 三个子目录被引用，是这一层共用的私有 API，
            # 所以单独放在 internal/，而不是跟着某一个 .c 走。
            "${CMAKE_CURRENT_SOURCE_DIR}/internal")
emaster_enable_strict_warnings(emaster_soem_control_session)
