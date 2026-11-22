cmake_minimum_required(VERSION 3.15)
project(rollback_fighter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(raylib REQUIRED)

# Warnings and sanitizers shared by every target (game and tests).
set(WARNINGS
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

function(apply_project_flags target)
    target_include_directories(${target} PRIVATE src)
    target_compile_options(${target} PRIVATE ${WARNINGS} $<$<CONFIG:Debug>:${SANITIZERS}>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:${SANITIZERS}>)
endfunction()

# --------------------------------------------------------
# Game
# --------------------------------------------------------
add_executable(game src/main.c src/sim/sim.c src/rollback/rollback.c src/net/packet.c src/net/net.c src/net/netsim.c)
target_link_libraries(game PRIVATE raylib)
apply_project_flags(game)

# --------------------------------------------------------
# Tests: build, then run with `ctest --test-dir build`
# --------------------------------------------------------
enable_testing()

add_executable(packet_test tests/packet_test.c src/net/packet.c)
apply_project_flags(packet_test)
add_test(NAME packet COMMAND packet_test)
