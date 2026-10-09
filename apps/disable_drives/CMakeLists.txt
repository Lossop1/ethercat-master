add_executable(emaster-disable main.c)

# 与 emaster-scan 同理：只依赖 SOEM 本身，不读部署档、不建会话。
# 主站起不来、或者从站已经掉出 OP 的时候，它照样能把驱动器禁掉 ——
# 而那正是最需要禁用的时刻。
target_link_libraries(emaster-disable PRIVATE soem)
target_compile_features(emaster-disable PRIVATE c_std_11)
emaster_enable_strict_warnings(emaster-disable)
