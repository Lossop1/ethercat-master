cmake_minimum_required(VERSION 3.10)

add_subdirectory(master)

add_executable(diagnose_slave diagnose_slave.c)
target_link_libraries(diagnose_slave soem pthread)
target_include_directories(diagnose_slave PRIVATE
    ${CMAKE_SOURCE_DIR}/external/soem/soem
    ${CMAKE_SOURCE_DIR}/external/soem/osal
    ${CMAKE_SOURCE_DIR}/external/soem/osal/linux
    ${CMAKE_SOURCE_DIR}/external/soem/oshw/linux
)
