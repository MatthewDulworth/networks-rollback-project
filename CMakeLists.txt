cmake_minimum_required(VERSION 3.15)
project(rollback_fighter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(raylib REQUIRED)

add_executable(game src/main.c)
target_link_libraries(game PRIVATE raylib)
target_compile_options(game PRIVATE -Wall -Wextra)
