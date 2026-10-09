cmake_minimum_required(VERSION 3.12)

add_executable(emaster-move emaster_move.c)
# 唯一需要包含路径的工程头文件是 command_socket_path.h（套接字路径的唯一构造点）。
# 这个目标不链接任何工程库，所以包含路径拿不到传递依赖，得显式给——status_client 同理。
target_include_directories(emaster-move PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(emaster-move PRIVATE c_std_11)
target_compile_options(emaster-move PRIVATE -Wall -Wextra -Werror)

install(TARGETS emaster-move DESTINATION bin)
