# =========================================================================
# DragonGL - standalone unit-test project (tests/)
#
# This directory is SELF-CONTAINED: the main project's CMakeLists.txt has
# no reference to it anymore. Build and run the suite independently:
#
#   cmake -B build-tests tests
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
#
# The test sources live here; the code under test is pulled straight from
# ../src (rules.c, pathfinding.c, client_tome.c) and the headers from
# ../include, so the tests always exercise the real production sources.
# Compile settings mirror the main project (clang, C11, same warning
# flags) so a warning can never hide behind different flags.
# =========================================================================

cmake_minimum_required(VERSION 3.16)
# Same compiler policy as the main CMakeLists.txt: clang by default, but
# never overriding an explicit choice (-DCMAKE_C_COMPILER or $CC).
option(DRAGONGL_USE_CLANG "Default to clang as the C compiler (overridable)" ON)
if(DRAGONGL_USE_CLANG AND NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/DragonGL.cmake")
project(DragonGLTests VERSION ${DRAGONGL_VERSION} LANGUAGES C)

# Same flags as the main project (cmake/DragonGL.cmake).
dragongl_compile_settings()

# Project roots (this file is tests/CMakeLists.txt)
set(DG_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DG_SRCS   "${DG_ROOT}/src")
set(DG_INC    "${DG_ROOT}/include")

# Include paths: the shared headers (include/) plus the client and server
# source dirs, where client_tome.h / pathfinding.h live.
include_directories(${DG_INC} ${DG_SRCS}/client ${DG_SRCS}/server)

enable_testing()
find_package(Threads REQUIRED)

# --- rules engine (dice, modifiers, damage, conditions) ---
add_executable(test_rules test_rules.c ${DG_SRCS}/shared/rules.c)
target_link_libraries(test_rules m)
add_test(NAME rules COMMAND test_rules)

# --- A* pathfinding (fixed hand-built maps) ---
add_executable(test_pathfinding test_pathfinding.c ${DG_SRCS}/server/pathfinding.c)
target_link_libraries(test_pathfinding m)
add_test(NAME pathfinding COMMAND test_pathfinding)

# --- wire protocol: encode/decode round trips and malformed payloads ---
add_executable(test_protocol test_protocol.c ${DG_SRCS}/shared/wire.c ${DG_SRCS}/shared/binio.c)
add_test(NAME protocol COMMAND test_protocol)

# --- floating tome animation (shop bounds, random wander, rotation) ---
add_executable(test_tome test_tome.c ${DG_SRCS}/client/client_tome.c)
target_link_libraries(test_tome m)
add_test(NAME tome COMMAND test_tome)

# --- VFX/FCT pool concurrency (B1 regression: net-thread spawn vs
# --- render-thread update/draw, now guarded by g_particles_mutex/g_fct_mutex)
add_executable(test_vfx_threads test_vfx_threads.c
    ${DG_SRCS}/client/client_particles.c
    ${DG_SRCS}/client/client_fct.c)
target_link_libraries(test_vfx_threads m Threads::Threads)
add_test(NAME vfx_threads COMMAND test_vfx_threads)

# --- GPU mesh generation (GPD): headless Vulkan compute test.
# Verifies the cull and expand stages against a CPU reference, each in
# its own stage (see tests/gpu_mesh_test.c). It SKIPS itself (exit 77)
# when no Vulkan ICD is present (e.g. CI runners) or the main build's
# compiled .spv files are missing. The reference is compiled with
# -ffp-contract=off so its mul/add chains match the (non-FMA-contracted)
# shaders bit-for-bit.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_executable(test_gpu_mesh gpu_mesh_test.c)
    target_link_libraries(test_gpu_mesh Vulkan::Vulkan m)
    set_source_files_properties(gpu_mesh_test.c PROPERTIES
        COMPILE_FLAGS "-ffp-contract=off")
    set(DRAGONGL_TEST_SHADER_DIR "${DG_ROOT}/build/shaders" CACHE PATH
        "Compiled shaders for the gpu_mesh test (skipped if absent)")
    add_test(NAME gpu_mesh COMMAND test_gpu_mesh ${DRAGONGL_TEST_SHADER_DIR})
    set_tests_properties(gpu_mesh PROPERTIES SKIP_RETURN_CODE 77)
endif()
