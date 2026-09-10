add_executable(emaster-move-deg emaster_move_deg.c)
target_link_libraries(emaster-move-deg PRIVATE emaster_bus)
install(TARGETS emaster-move-deg DESTINATION bin)
