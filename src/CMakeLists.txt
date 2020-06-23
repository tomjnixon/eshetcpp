add_executable(eshet cli.cpp utils.cpp)
target_link_libraries(eshet PRIVATE eshetcpp CLI11 rapidjson)

add_executable(eshet_complete complete.cpp utils.cpp)
target_link_libraries(eshet_complete PRIVATE eshetcpp CLI11 rapidjson)

install(TARGETS eshet eshet_complete DESTINATION bin)
