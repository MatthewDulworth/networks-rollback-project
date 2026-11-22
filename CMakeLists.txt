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
# Everything that runs a match without raylib: the sim, rollback, the
# network stack and the per-frame loop. Shared by the game and the netcode
# test, so the test exercises exactly the code the game runs.
set(CORE_SOURCES
    src/match.c
    src/sim/sim.c
    src/rollback/rollback.c
    src/rollback/sync_check.c
    src/rollback/desync_check.c
    src/net/packet.c
    src/net/net.c
    src/net/netsim.c
    src/net/session.c
)

add_executable(game src/main.c src/args.c src/input.c src/render/render.c ${CORE_SOURCES})
target_link_libraries(game PRIVATE raylib)
apply_project_flags(game)

# --------------------------------------------------------
# Tests: build, then run with `ctest --test-dir build`
# --------------------------------------------------------
enable_testing()

add_executable(packet_test tests/packet_test.c src/net/packet.c)
apply_project_flags(packet_test)
add_test(NAME packet COMMAND packet_test)

# Two real peers per scenario over localhost UDP, with simulated lag and
# loss, running the game's own per-frame loop (match.c). ~10 s, real time.
add_executable(netcode_test tests/netcode_test.c ${CORE_SOURCES})
apply_project_flags(netcode_test)
add_test(NAME netcode COMMAND netcode_test)
set_tests_properties(netcode PROPERTIES TIMEOUT 60)
