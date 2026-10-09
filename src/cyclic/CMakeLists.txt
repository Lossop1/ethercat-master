# clock.c 走 clock_nanosleep 的绝对时基等待，MinGW 上没有这个调用，因此只在 Linux 上建。
# 这就是它原先留在 src/bus/soem/ 的唯一原因——它要的是 POSIX，不是 SOEM。
set(_emaster_cyclic_sources exchange.c histogram.c timing.c window.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _emaster_cyclic_sources clock.c)
endif()
add_library(emaster_cyclic STATIC ${_emaster_cyclic_sources})
add_library(emaster::cyclic ALIAS emaster_cyclic)

target_compile_features(emaster_cyclic PUBLIC c_std_11)
target_include_directories(
    emaster_cyclic
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
        "$<INSTALL_INTERFACE:include>")
emaster_enable_strict_warnings(emaster_cyclic)
