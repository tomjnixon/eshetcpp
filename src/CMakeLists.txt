add_executable(eshet cli.cpp utils.cpp)
target_link_libraries(eshet PRIVATE eshetcpp CLI11 rapidjson)

add_executable(eshet_complete complete.cpp utils.cpp)
target_link_libraries(eshet_complete PRIVATE eshetcpp CLI11 rapidjson)

include(GNUInstallDirs)

install(TARGETS eshet eshet_complete DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES eshet_complete.bash
    DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
    RENAME eshet
)
