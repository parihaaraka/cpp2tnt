cmake_minimum_required(VERSION 3.10)

project(cpp2tnt C CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

include_directories(BEFORE third_party)
add_subdirectory(third_party/msgpuck)

set (CMAKE_CXX_STANDARD 17)
file(GLOB LIB_FILES *.cpp third_party/*.c)

add_library(cpp2tnt STATIC ${LIB_FILES})
target_include_directories(cpp2tnt PUBLIC .)

add_executable(example "tests/example.cpp")
target_link_libraries(example ${PROJECT_NAME} ev pthread msgpuck)
