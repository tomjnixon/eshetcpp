add_executable(eshet cli.cpp utils.cpp)
target_link_libraries(eshet PRIVATE eshetcpp CLI11 rapidjson)
