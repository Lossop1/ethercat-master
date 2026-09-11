add_executable(emaster-move-deg emaster_move_deg.c)
target_link_libraries(emaster-move-deg PRIVATE emaster::soem_control_session emaster::session emaster::config emaster::messages)
install(TARGETS emaster-move-deg DESTINATION bin)
