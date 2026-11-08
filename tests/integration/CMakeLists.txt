# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# Server integration tests (standalone CMake project).
#
#   cmake -B build-integration tests/integration -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
#   cmake --build build-integration --parallel
#   ctest --test-dir build-integration --output-on-failure
#
# Two kinds of tests are registered:
#   * network tests (test_server.py): a real dragongl_server process is
#     started on a free port and driven by a protocol-v3 client;
#   * in-process tests (server_harness): the server translation units are
#     linked without main() and exercised directly (persistence, trade).
#
# Tests that reproduce a known, still open defect are marked WILL_FAIL /
# expectedFailure with the issue id of the review (P0-x, P1-x). Fixing the
# defect turns them into an "unexpected success": remove the marker in the
# same commit as the fix.
cmake_minimum_required(VERSION 3.16)
project(DragonGLIntegration LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(DGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_compile_definitions(_DEFAULT_SOURCE)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(DGL_SERVER_SRCS
    ${DGL_ROOT}/src/server/main_server.c
    ${DGL_ROOT}/src/server/server_net.c
    ${DGL_ROOT}/src/server/server_conn.c
    ${DGL_ROOT}/src/server/ai.c
    ${DGL_ROOT}/src/server/aoe.c
    ${DGL_ROOT}/src/server/combat_log.c
    ${DGL_ROOT}/src/server/pathfinding.c
    ${DGL_ROOT}/src/server/server_combat.c
    ${DGL_ROOT}/src/server/server_spawn.c
    ${DGL_ROOT}/src/server/server_world.c
    ${DGL_ROOT}/src/server/server_commands.c
    ${DGL_ROOT}/src/server/spell_router.c
    ${DGL_ROOT}/src/server/tombstone.c
    ${DGL_ROOT}/src/shared/map.c
    ${DGL_ROOT}/src/shared/game.c
    ${DGL_ROOT}/src/shared/net.c
    ${DGL_ROOT}/src/shared/rules.c
    ${DGL_ROOT}/src/shared/cJSON.c
    ${DGL_ROOT}/src/shared/data_loader.c
)
set(DGL_INCLUDES ${DGL_ROOT}/include ${DGL_ROOT}/src/server ${DGL_ROOT}/src/client)

# --- The real server, built with the flags of this project (sanitizers) ---
add_executable(dragongl_server_it ${DGL_SERVER_SRCS})
target_include_directories(dragongl_server_it PRIVATE ${DGL_INCLUDES})
target_link_libraries(dragongl_server_it PRIVATE Threads::Threads m)

# --- In-process harness: same sources, no main() ---
add_executable(server_harness server_harness.c ${DGL_SERVER_SRCS})
target_include_directories(server_harness PRIVATE ${DGL_INCLUDES})
target_compile_definitions(server_harness PRIVATE DRAGONGL_NO_MAIN
    DGL_TEST_DATA_DIR="${DGL_ROOT}/data")
# Without main() a few file-local helpers and globals of main_server.c are unused.
target_compile_options(server_harness PRIVATE -Wno-unused-function -Wno-unused-variable)
target_link_libraries(server_harness PRIVATE Threads::Threads m)

enable_testing()

# The harness writes saves/ into its working directory: give it its own.
set(DGL_WORK "${CMAKE_CURRENT_BINARY_DIR}/work")
file(MAKE_DIRECTORY "${DGL_WORK}")

add_test(NAME persistence_full_backpack_roundtrip
         COMMAND server_harness full_backpack_roundtrip
         WORKING_DIRECTORY "${DGL_WORK}")
add_test(NAME persistence_partial_backpack_roundtrip
         COMMAND server_harness partial_backpack_roundtrip
         WORKING_DIRECTORY "${DGL_WORK}")
foreach(t trade_offer_revalidated_on_accept trade_moves_item_and_gold
          trade_into_full_backpack_is_cancelled)
    add_test(NAME ${t} COMMAND server_harness ${t} WORKING_DIRECTORY "${DGL_WORK}")
endforeach()

add_test(NAME server_network
         COMMAND ${Python3_EXECUTABLE} -m unittest -v test_server
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(server_network PROPERTIES
    ENVIRONMENT "DRAGONGL_SERVER=$<TARGET_FILE:dragongl_server_it>;DRAGONGL_DATA=${DGL_ROOT}/data;ASAN_OPTIONS=detect_leaks=0;PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 900)
