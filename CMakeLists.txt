cmake_minimum_required(VERSION 3.10)

project(cpp2tnt C CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

add_subdirectory(third_party/msgpuck)

set (CMAKE_CXX_STANDARD 20)
file(GLOB LIB_FILES *.cpp third_party/*.c)
if(NOT DEFINED CPP2TNT_LIBEV)
    message("libev-related sources disabled (define CPP2TNT_LIBEV to enable it)")
    list(FILTER LIB_FILES EXCLUDE REGEX ".*/ev4cpp2tnt.cpp$")
endif()

add_library(${PROJECT_NAME} STATIC ${LIB_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC . third_party)
target_link_libraries(${PROJECT_NAME} msgpuck)

