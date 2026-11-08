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
include("${CMAKE_CURRENT_LIST_DIR}/cmake/DragonGL.cmake")
project(DragonGL VERSION ${DRAGONGL_VERSION} LANGUAGES C)

# What to build. A headless server host needs neither Vulkan, GLFW nor
# glslc: -DDRAGONGL_BUILD_CLIENT=OFF.
option(DRAGONGL_BUILD_SERVER "Build the game server (dragongl_server)" ON)
option(DRAGONGL_BUILD_CLIENT "Build the Vulkan client (dragongl_client) and its shaders" ON)
option(DRAGONGL_INSTALL_SYSTEMD_UNIT "Install the systemd unit of the server" ON)
set(DRAGONGL_SYSTEMD_UNITDIR "lib/systemd/system" CACHE PATH
    "Where the systemd unit goes (relative to the prefix, or absolute)")
if(NOT DRAGONGL_BUILD_SERVER AND NOT DRAGONGL_BUILD_CLIENT)
    message(FATAL_ERROR "DRAGONGL_BUILD_SERVER and DRAGONGL_BUILD_CLIENT are both OFF: nothing to build")
endif()

dragongl_compile_settings()
include(GNUInstallDirs)
set(DRAGONGL_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
dragongl_generate_config("${DRAGONGL_GENERATED_DIR}")
message(STATUS "Dragon GL ${DRAGONGL_VERSION} (server: ${DRAGONGL_BUILD_SERVER}, client: ${DRAGONGL_BUILD_CLIENT})")

# --- Dependencies -----------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
if(DRAGONGL_BUILD_SERVER)
    # libsodium: Argon2id password hashing and constant-time comparisons
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
endif()
if(DRAGONGL_BUILD_CLIENT)
    find_package(Vulkan REQUIRED)
    # GLFW: window and Vulkan surface of the client
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
endif()

include_directories(include "${DRAGONGL_GENERATED_DIR}")

if(DRAGONGL_BUILD_CLIENT)
    # --- Shaders (SPIR-V) ---
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
endif()

# --- Targets ----------------------------------------------------------------------
set(COMMON_LIBS Threads::Threads m)

# Shared sources (networking, rules, map, data loading, wire protocol)
set(SHARED_SRCS
    src/shared/map.c
    src/shared/game.c
    src/shared/net.c
    src/shared/rules.c
    src/shared/cJSON.c
    src/shared/data_loader.c
    src/shared/binio.c
    src/shared/wire.c
)
set_source_files_properties(src/shared/cJSON.c PROPERTIES COMPILE_OPTIONS "-Wno-keyword-macro")

if(DRAGONGL_BUILD_SERVER)
    add_executable(dragongl_server
        src/server/main_server.c
        src/server/server_net.c
        src/server/server_conn.c
        src/server/auth.c
        src/server/persist.c
        src/server/writer.c
        src/server/login_worker.c
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
    target_link_libraries(dragongl_server ${COMMON_LIBS} PkgConfig::SODIUM)
endif()

if(DRAGONGL_BUILD_CLIENT)
    add_executable(dragongl_client
        src/client/main_client.c
        src/client/net_thread.c
        src/client/cli_thread.c
        src/client/client_fct.c
        src/client/client_minimap.c
        src/client/client_particles.c
        src/client/client_tome.c
        src/client/render_vk.c
        src/client/vk_init.c
        ${SHARED_SRCS}
    )
    target_include_directories(dragongl_client PRIVATE src/client)
    target_link_libraries(dragongl_client ${COMMON_LIBS} Vulkan::Vulkan PkgConfig::GLFW)
    add_dependencies(dragongl_client dragongl_shaders)
endif()

# --- Installed paths, substituted into the man pages and the systemd unit ----------
set(DRAGONGL_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(DRAGONGL_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(DRAGONGL_PKGDATADIR "${CMAKE_INSTALL_FULL_DATADIR}/dragongl")

# --- Man pages: version and date come from dragongl.spec -------------------------
function(dragongl_man_page name)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/man/${name}.in"
                   "${CMAKE_BINARY_DIR}/man/${name}" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/man/${name}" DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endfunction()

# --- Installation -------------------------------------------------------------------
# Every installed file has one owner in dragongl.spec:
#   dragongl         client, its launcher, man pages and shaders
#   dragongl-server  server, launchers, man pages, systemd unit and config
#   dragongl-data    game data (JSON)
#   dragongl-tools   DM and balance tools, Data Studio
if(DRAGONGL_BUILD_SERVER)
    install(TARGETS dragongl_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS dragongl-server dragongl-reset-world DESTINATION ${CMAKE_INSTALL_BINDIR})
    dragongl_man_page(dragongl_server.1)
    dragongl_man_page(dragongl-server.1)
    dragongl_man_page(dragongl-reset-world.1)

    # Game data (read-only JSON). The server writes only to its state
    # directory, never here.
    install(FILES data/bestiary.json data/items.json data/spells.json
            DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/data)

    # DM and balance tools (dm_pdf runs generate_pdf_map.py from here).
    install(PROGRAMS tools/analyze_balance.py tools/generate_pdf_map.py
            DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools)
    dragongl_man_page(analyze_balance.1)
    dragongl_man_page(generate_pdf_map.1)
    # Data Studio (self-contained HTML workbenches): the pages load the
    # datasets through ../../data/*.json, i.e. <datadir>/dragongl/data.
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

    if(DRAGONGL_INSTALL_SYSTEMD_UNIT)
        configure_file(packaging/dragongl-server.service.in
                       "${CMAKE_BINARY_DIR}/packaging/dragongl-server.service" @ONLY)
        install(FILES "${CMAKE_BINARY_DIR}/packaging/dragongl-server.service"
                DESTINATION ${DRAGONGL_SYSTEMD_UNITDIR})
        # Passwords and options of the service: root-only, never replaced
        # by an update (the spec marks it %config(noreplace)).
        install(FILES packaging/dragongl-server.conf
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/dragongl
                PERMISSIONS OWNER_READ OWNER_WRITE)
    endif()
endif()

if(DRAGONGL_BUILD_CLIENT)
    install(TARGETS dragongl_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS dragongl-client DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${SHADER_SPV_FILES} DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/shaders)
    dragongl_man_page(dragongl_client.1)
    dragongl_man_page(dragongl-client.1)
endif()
