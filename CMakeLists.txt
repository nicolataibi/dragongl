cmake_minimum_required(VERSION 3.10)
set(CMAKE_C_COMPILER "clang")
project(DragonGL VERSION 2026.05.26.01 LANGUAGES C)

# --- Standard C e Opzioni di Compilazione ---
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Fix OpenGL Policy CMP0072
cmake_policy(SET CMP0072 NEW)
set(OpenGL_GL_PREFERENCE GLVND)

# Definizioni globali
add_compile_definitions(_DEFAULT_SOURCE)

# Flag di sicurezza e avvisi (Fedora Compliance)
if(MSVC)
    add_compile_options(/W4)
else()
    # Enable PIE globally
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_compile_options(-fPIE)
    add_link_options(-pie)

    add_compile_options(-Wall -Wextra -Wpedantic -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security)

    # Add optimization and debug info based on build type
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g)
    endif()

    add_link_options(-Wl,-z,relro,-z,now)
endif()

# --- Ricerca Dipendenze ---
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)

# GLFW per il client 3D (backend OpenGL e Vulkan)
pkg_check_modules(GLFW REQUIRED glfw3)

# --- Include Directories ---
include_directories(include src/client ${GLFW_INCLUDE_DIRS})

# --- Helper per Compilazione Shader Vulkan ---
find_program(GLSLC_EXECUTABLE glslc)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc compiler not found. Required for Vulkan shaders.")
endif()

set(SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shader.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shader.frag"
)
set(SPV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPV_OUTPUT_DIR})

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_FILE "${SPV_OUTPUT_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER} -o ${SPV_FILE}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SHADER_SPV_FILES ${SPV_FILE})
endforeach()

add_custom_target(dragongl_shaders ALL DEPENDS ${SHADER_SPV_FILES})

# --- Definizione Target ---

set(COMMON_LIBS Threads::Threads m)

# Sorgenti condivisi (networking, logica e generazione mappa)
set(SHARED_SRCS
    src/shared/map.c
    src/shared/game.c
    src/shared/net.c
    src/shared/rules.c
    src/shared/cJSON.c
    src/shared/data_loader.c
)

# 1. dragongl_server

set_source_files_properties(src/shared/cJSON.c PROPERTIES COMPILE_FLAGS "-Wno-keyword-macro")
add_executable(dragongl_server
    src/server/main_server.c
    src/server/ai.c
    src/server/aoe.c
    src/server/combat_log.c
    src/server/pathfinding.c
    src/server/server_combat.c
    src/server/server_spawn.c
    src/server/server_world.c
    src/server/server_commands.c
    src/server/spell_router.c
    src/server/tombstone.c
    ${SHARED_SRCS}
)
target_link_libraries(dragongl_server ${COMMON_LIBS})

# 2. dragongl_client
set(CLIENT_SRCS
    src/client/main_client.c
    src/client/net_thread.c
    src/client/cli_thread.c
    src/client/client_fct.c
    src/client/client_minimap.c
    src/client/client_particles.c
    src/client/render_gl.c
    src/client/render_vk.c
    src/client/vk_init.c
)

add_executable(dragongl_client
    ${CLIENT_SRCS}
    ${SHARED_SRCS}
)
target_link_libraries(dragongl_client
    ${COMMON_LIBS}
    OpenGL::GL
    GLU
    Vulkan::Vulkan
    ${GLFW_LIBRARIES}
)
add_dependencies(dragongl_client dragongl_shaders)

# --- Installazione ---

include(GNUInstallDirs)

install(TARGETS dragongl_server dragongl_client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(PROGRAMS dragongl-server dragongl-client dragongl_reset_world
        DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${SHADER_SPV_FILES}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/shaders)

# --- Man Pages ---
set(MAN_PAGES
    man/dragongl_server.1
    man/dragongl_client.1
    man/dragongl-server.1
    man/dragongl-client.1
    man/dragongl_reset_world.1
    man/analyze_balance.1
    man/generate_pdf_map.1
)
install(FILES ${MAN_PAGES}
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# --- Developer Tools ---
install(PROGRAMS tools/analyze_balance.py tools/generate_pdf_map.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools)
