cmake_minimum_required(VERSION 3.12)

add_executable(emaster-watch emaster_watch.c)
target_compile_features(emaster-watch PRIVATE c_std_11)
target_compile_options(emaster-watch PRIVATE -Wall -Wextra -Werror)

install(TARGETS emaster-watch DESTINATION bin)
