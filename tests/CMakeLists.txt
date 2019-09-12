cmake_minimum_required(VERSION 3.10)

project(cpp2tnt_example C CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

set(CMAKE_CXX_STANDARD 17)

add_executable(${PROJECT_NAME} "example.cpp")
add_subdirectory("${CMAKE_SOURCE_DIR}/.." build)
target_link_libraries(${PROJECT_NAME} cpp2tnt ev pthread msgpuck)

