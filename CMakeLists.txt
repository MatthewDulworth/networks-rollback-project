cmake_minimum_required(VERSION 3.15)
project(rollback_fighter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(raylib REQUIRED)

add_executable(game src/main.c src/sim/sim.c src/rollback/rollback.c)
target_include_directories(game PRIVATE src)
target_link_libraries(game PRIVATE raylib)
target_compile_options(game PRIVATE
    -Wall -Wextra
    # Promote warnings that are almost always real bugs in C to errors.
    -Werror=incompatible-pointer-types
    -Werror=int-conversion
    -Werror=implicit-function-declaration
    -Werror=return-type
)

# Debug builds only: AddressSanitizer catches out-of-bounds and use-after-free
# memory access, UndefinedBehaviorSanitizer catches things like signed
# overflow. Both stop at the faulting line. Runs ~2x slower.
set(SANITIZERS -fsanitize=address,undefined -fno-omit-frame-pointer)
target_compile_options(game PRIVATE $<$<CONFIG:Debug>:${SANITIZERS}>)
target_link_options(game PRIVATE $<$<CONFIG:Debug>:${SANITIZERS}>)
