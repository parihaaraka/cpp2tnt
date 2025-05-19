cmake_minimum_required(VERSION 3.10)

project(cpp2tnt_tests C CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

set(CMAKE_CXX_STANDARD 20)

add_compile_definitions(MP_READER2)

file(GLOB SRC_FILES *.cpp "${CMAKE_SOURCE_DIR}/../third_party/*.c")
add_executable(${PROJECT_NAME} ${SRC_FILES})
set(CPP2TNT_LIBEV true) # compile libev wrapper
add_subdirectory("${CMAKE_SOURCE_DIR}/.." build)
target_link_libraries(${PROJECT_NAME} cpp2tnt ev pthread)
