cmake_minimum_required(VERSION 3.12)

add_executable(emaster-move emaster_move.c)
target_compile_features(emaster-move PRIVATE c_std_11)
target_compile_options(emaster-move PRIVATE -Wall -Wextra -Werror)

install(TARGETS emaster-move DESTINATION bin)
