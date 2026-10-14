add_executable(emaster-scan main.c)

# 只依赖 SOEM 本身：不读部署档、不建会话，所以不链 emaster::* 那些库。
# 这正是它有用的原因——主站起不来的时候它照样能回答"环上有几台"。
target_link_libraries(emaster-scan PRIVATE soem)
# 本目标不链工程库，拿不到传递包含路径；cli.h 要 include/ 与生成目录里的 version.h。
target_include_directories(
    emaster-scan PRIVATE "${CMAKE_SOURCE_DIR}/include" "${EMASTER_GENERATED_DIR}")
target_compile_features(emaster-scan PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-scan)
