add_library(CLI11 INTERFACE)
target_include_directories(CLI11 INTERFACE ${PROJECT_SOURCE_DIR}/submodules/CLI11)

add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE ${PROJECT_SOURCE_DIR}/submodules/rapidjson/include)
