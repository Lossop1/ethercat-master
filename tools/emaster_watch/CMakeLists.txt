cmake_minimum_required(VERSION 3.12)

add_executable(emaster-watch emaster_watch.c)
# 同上：唯一需要的工程头文件是 command_socket_path.h，本目标不链接工程库，
# 包含路径得显式给。
target_include_directories(emaster-watch PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(emaster-watch PRIVATE c_std_11)
target_compile_options(emaster-watch PRIVATE -Wall -Wextra -Werror)

install(TARGETS emaster-watch DESTINATION bin)
