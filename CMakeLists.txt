cmake_minimum_required(VERSION 3.9)

if(DEFINED PROJECT_NAME)
  set(SUBPROJECT ON)
endif()

project(eshetcpp VERSION 0.0.1 LANGUAGES CXX)

add_subdirectory(submodules)

FIND_PACKAGE(Threads REQUIRED)

add_library(eshetcpp INTERFACE)
target_include_directories(eshetcpp
    INTERFACE
  ${PROJECT_SOURCE_DIR}/include
  INTERFACE
  ${PROJECT_SOURCE_DIR}/submodules/msgpack-c/include
)
# set_property(TARGET eshetcpp PROPERTY CXX_STANDARD 17)
target_compile_features(eshetcpp INTERFACE cxx_std_17)
target_link_libraries (eshetcpp INTERFACE Threads::Threads)
target_link_libraries (eshetcpp INTERFACE actorpp)

if(NOT SUBPROJECT)
    include(CTest)
    add_subdirectory(test)

    add_subdirectory(src)
endif()
