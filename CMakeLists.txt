cmake_minimum_required(VERSION 3.16)
# --- C compiler selection (L4) ----------------------------------------
# The compiler is a CHOICE, not a force: the old `set(CMAKE_C_COMPILER
# "clang")` silently overrode ANY user-supplied compiler, which a
# top-level project must not do (foreign toolchains / RPM wrappers /
# explicit -DCMAKE_C_COMPILER=gcc were all clobbered). Now:
#   -DRAGONGL_USE_CLANG=ON (default): clang is picked only if the user
#     did not already choose one (-DCMAKE_C_COMPILER=... or $CC wins);
#   -DRAGONGL_USE_CLANG=OFF: the toolchain decides entirely.
option(DRAGONGL_USE_CLANG "Default to clang as the C compiler (overridable)" ON)
if(DRAGONGL_USE_CLANG AND NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
project(DragonGL VERSION 2026.09.16.01 LANGUAGES C)

# --- Standard C e Opzioni di Compilazione ---
# C11 (not C23): the codebase uses no C23-specific feature, and C11 is
# the portable floor for the RPM toolchain (typedef redefinition, stdatomic
# and other C23 corners are not guaranteed on older distro compilers).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# OpenGL policy: cmake_minimum_required(3.16) already sets CMP0072 to
# NEW (the policy was introduced in 3.9), so the explicit
# cmake_policy() is redundant — the 3.16 floor is the fix (L4).
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

    add_compile_options(-Wall -Wextra -Wpedantic -fstack-protector-strong -Wformat -Wformat-security)

    # Add optimization and debug info based on build type
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
        # _FORTIFY_SOURCE needs optimization to do anything: with -O0 it
        # is inert and some compilers/glibc versions even warn about it
        # (L4), so it belongs to Release only.
        add_compile_options(-D_FORTIFY_SOURCE=2)
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
    # GPU-driven path (GPD): GPU culling passes, mesh allocation/expand
    # passes, the indirect-command writer and the SSBO-based scene vertex
    # shader (common code in shaders/gpd_mesh.glsl + gpd_ops.glsl)
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_cull_map.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_cull_dyn.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_expand_map.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_expand_dyn.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_expand_oriented.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_final.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_scene.vert"
)
set(SPV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPV_OUTPUT_DIR})

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_FILE "${SPV_OUTPUT_DIR}/${SHADER_NAME}.spv")
    # The GPD compute shaders #include the shared gpd_mesh.glsl: recompile
    # them when it changes, and give glslc the include dir.
    if(SHADER MATCHES "gpd_(cull_map|cull_dyn|expand_map|expand_dyn|expand_oriented)\.comp$")
        set(SHADER_EXTRA_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_mesh.glsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpd_ops.glsl")
    else()
        set(SHADER_EXTRA_DEPENDS "")
    endif()
    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLC_EXECUTABLE} -I${CMAKE_CURRENT_SOURCE_DIR}/shaders ${SHADER} -o ${SPV_FILE}
        DEPENDS ${SHADER} ${SHADER_EXTRA_DEPENDS}
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
    src/server/server_net.c
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
    src/client/client_tome.c
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
    OpenGL::GLU
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

# --- Game data (read-only JSON datasets) ---
# The server resolves its data dir at start: ./data (development) or
# /usr/share/dragongl/data (RPM). Install ONLY the JSON files — the
# mutable world state (world.dat, npcs.dat, artifacts.dat) belongs to
# the user's data dir, never to the package.
install(FILES data/bestiary.json data/items.json data/spells.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/data)

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

# --- Data Studio (self-contained HTML CRUD workbenches) ---
# Installed under <datadir>/dragongl/tools/web: the pages load their
# databases through the relative path ../../data/*.json, which resolves
# to <datadir>/dragongl/data — the same location where the datasets
# above are installed. The `_`-prefixed files are source fragments for
# rebuilding (see tools/web/build.py and tools/web/README.md).
install(PROGRAMS tools/web/build.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools/web)
install(FILES
    tools/web/menu.html
    tools/web/bestiary.html
    tools/web/items.html
    tools/web/spells.html
    tools/web/README.md
    tools/web/_core.js
    tools/web/_design.css
    tools/web/_editor-shell.html
    tools/web/_mod-bestiary.js
    tools/web/_mod-items.js
    tools/web/_mod-spells.js
    tools/web/_menu.html
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools/web)
