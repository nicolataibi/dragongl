# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# libFuzzer targets (standalone CMake project, clang only).
#
#   cmake -B build-fuzz tests/fuzz -DCMAKE_C_COMPILER=clang
#   cmake --build build-fuzz --parallel
#   tools/quality.sh fuzz 60        # seeds, then 60 s per target
#
# Targets, each built with libFuzzer, ASan and UBSan:
#   fuzz_wire      one protocol frame -> the decoder of its type (wire.c)
#   fuzz_persist   a save-file payload -> the reader of its kind (persist.c)
#   fuzz_text_cmd  a MSG_TEXT_CMD payload -> handle_text_cmd()
# DGL_FUZZ_SEEDS=<dir> ./<target> writes the seed corpus of a target from
# the encoders of this tree and exits.
cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DragonGL.cmake")
project(DragonGLFuzz VERSION ${DRAGONGL_VERSION} LANGUAGES C)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzz targets need clang (libFuzzer): -DCMAKE_C_COMPILER=clang")
endif()

# Debug keeps assert() active; -O1 below keeps the targets fast.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(DRAGONGL_HARDENING OFF)
dragongl_compile_settings()
dragongl_generate_config("${CMAKE_CURRENT_BINARY_DIR}/generated")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(OpenSSL 3.0 REQUIRED)

set(FUZZ_FLAGS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer -g -O1)
add_compile_options(${FUZZ_FLAGS})
add_link_options(-fsanitize=fuzzer,address,undefined)

set(FUZZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(FIXTURE_DIR "${DRAGONGL_ROOT}/tests/integration")
set(FUZZ_INCLUDES ${DRAGONGL_ROOT}/include ${DRAGONGL_ROOT}/src/server ${FIXTURE_DIR} ${FUZZ_DIR}
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The wire decoders need only the protocol code.
add_executable(fuzz_wire fuzz_wire.c fuzz_common.c
    ${DRAGONGL_ROOT}/src/shared/wire.c ${DRAGONGL_ROOT}/src/shared/binio.c)
target_include_directories(fuzz_wire PRIVATE ${FUZZ_INCLUDES})

# The readers and the commands run inside the in-process server fixture.
foreach(t fuzz_persist fuzz_text_cmd)
    add_executable(${t} ${t}.c fuzz_common.c ${FIXTURE_DIR}/fixture.c
        ${DRAGONGL_SERVER_LIB_SRCS} ${DRAGONGL_SHARED_SRCS})
    target_include_directories(${t} PRIVATE ${FUZZ_INCLUDES})
    target_compile_definitions(${t} PRIVATE DGL_FUZZ_DATA_DIR="${DRAGONGL_ROOT}/data")
    target_link_libraries(${t} PRIVATE Threads::Threads m PkgConfig::SODIUM OpenSSL::SSL OpenSSL::Crypto)
endforeach()
