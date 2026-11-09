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
#     started on a free port and driven by a protocol client (dglproto.py);
#   * in-process tests (server_harness): the server translation units are
#     linked without main() and exercised directly (persistence, trade).
#
# Tests that reproduce a known, still open defect are marked WILL_FAIL /
# expectedFailure with the issue id of the review (P0-x, P1-x). Fixing the
# defect turns them into an "unexpected success": remove the marker in the
# same commit as the fix.
cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DragonGL.cmake")
project(DragonGLIntegration VERSION ${DRAGONGL_VERSION} LANGUAGES C)

dragongl_compile_settings()
set(DGL_ROOT "${DRAGONGL_ROOT}")
dragongl_generate_config("${CMAKE_CURRENT_BINARY_DIR}/generated")

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(OpenSSL 3.0 REQUIRED)

set(DGL_SERVER_SRCS ${DRAGONGL_SERVER_SRCS} ${DRAGONGL_SHARED_SRCS})
set(DGL_INCLUDES ${DGL_ROOT}/include ${DGL_ROOT}/src/server ${DGL_ROOT}/src/client
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# --- The real server, built with the flags of this project (sanitizers) ---
add_executable(dragongl_server_it ${DGL_SERVER_SRCS})
target_include_directories(dragongl_server_it PRIVATE ${DGL_INCLUDES})
target_link_libraries(dragongl_server_it PRIVATE Threads::Threads m PkgConfig::SODIUM OpenSSL::SSL OpenSSL::Crypto)

# --- In-process harness: same sources, no main() ---
add_executable(server_harness server_harness.c fixture.c ${DGL_SERVER_SRCS})
target_include_directories(server_harness PRIVATE ${DGL_INCLUDES})
target_compile_definitions(server_harness PRIVATE DRAGONGL_NO_MAIN
    DGL_TEST_DATA_DIR="${DGL_ROOT}/data")
# Without main() a few file-local helpers and globals of main_server.c are unused.
target_compile_options(server_harness PRIVATE -Wno-unused-function -Wno-unused-variable)
target_link_libraries(server_harness PRIVATE Threads::Threads m PkgConfig::SODIUM OpenSSL::SSL OpenSSL::Crypto)

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
          trade_into_full_backpack_is_cancelled world_roundtrip npcs_roundtrip
          tombstone_and_flags_roundtrip writer_queue login_job_outcomes
          login_worker_async command_table command_words doors
          transit_cantrips_need_a_book
          floor_walk_matches_scan entity_lookup player_move)
    add_test(NAME ${t} COMMAND server_harness ${t} WORKING_DIRECTORY "${DGL_WORK}")
endforeach()

# The Python codec of the protocol table against the generated vectors.
add_test(NAME protocol_vectors
         COMMAND ${Python3_EXECUTABLE} -m unittest -v test_protocol_vectors
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(protocol_vectors PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME server_network
         COMMAND ${Python3_EXECUTABLE} -m unittest -v test_server
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(server_network PROPERTIES
    ENVIRONMENT "DRAGONGL_SERVER=$<TARGET_FILE:dragongl_server_it>;DRAGONGL_DATA=${DGL_ROOT}/data;ASAN_OPTIONS=detect_leaks=0;PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 900)
