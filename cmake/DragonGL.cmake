# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# Settings shared by the main project, the unit tests (tests/) and the
# server integration tests (tests/integration).

include_guard(GLOBAL)

get_filename_component(DRAGONGL_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# --- Version: one source, the Version: line of dragongl.spec ----------------
# YYYY.MM.DD.NN. The man pages, the binaries (--version) and the package all
# take it from here; tools/check_release.py checks CHANGELOG.md against it.
file(STRINGS "${DRAGONGL_ROOT}/dragongl.spec" _dgl_version_line REGEX "^Version:")
string(REGEX REPLACE "^Version:[ \t]*([0-9.]+).*$" "\\1" DRAGONGL_VERSION "${_dgl_version_line}")
if(NOT DRAGONGL_VERSION MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9])\\.([0-9][0-9])\\.[0-9][0-9]$")
    message(FATAL_ERROR "dragongl.spec: Version '${DRAGONGL_VERSION}' is not YYYY.MM.DD.NN")
endif()
set(DRAGONGL_RELEASE_DATE "${CMAKE_MATCH_1}-${CMAKE_MATCH_2}-${CMAKE_MATCH_3}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DRAGONGL_ROOT}/dragongl.spec")

# Protocol version (the man pages name the ALPN protocol): protocol/messages.json.
file(STRINGS "${DRAGONGL_ROOT}/protocol/messages.json" _dgl_proto_line REGEX "^  \"version\":")
string(REGEX REPLACE "^  \"version\": *([0-9]+).*$" "\\1" DRAGONGL_PROTOCOL_VERSION "${_dgl_proto_line}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DRAGONGL_ROOT}/protocol/messages.json")

# --- Source lists ----------------------------------------------------------------
# One list each, used by the main project, the integration tests and the fuzz
# targets, so a new translation unit is added in one place.
set(DRAGONGL_SHARED_SRCS
    "${DRAGONGL_ROOT}/src/shared/map.c"
    "${DRAGONGL_ROOT}/src/shared/map_city.c"
    "${DRAGONGL_ROOT}/src/shared/map_traps.c"
    "${DRAGONGL_ROOT}/src/shared/net.c"
    "${DRAGONGL_ROOT}/src/shared/rules.c"
    "${DRAGONGL_ROOT}/src/shared/spell_effect.c"
    "${DRAGONGL_ROOT}/src/shared/spell_rules.c"
    "${DRAGONGL_ROOT}/src/shared/cJSON.c"
    "${DRAGONGL_ROOT}/src/shared/data_loader.c"
    "${DRAGONGL_ROOT}/src/shared/data_items.c"
    "${DRAGONGL_ROOT}/src/shared/binio.c"
    "${DRAGONGL_ROOT}/src/shared/histogram.c"
    "${DRAGONGL_ROOT}/src/shared/wire.c"
    "${DRAGONGL_ROOT}/src/shared/strnum.c"
    "${DRAGONGL_ROOT}/src/shared/tls_common.c"
)
# Every server module but main(): the server links them with
# main_server.c, the in-process test harness and the fuzzers with their own
# entry point.
set(DRAGONGL_SERVER_LIB_SRCS
    "${DRAGONGL_ROOT}/src/server/server_state.c"
    "${DRAGONGL_ROOT}/src/server/server_startup.c"
    "${DRAGONGL_ROOT}/src/server/server_login.c"
    "${DRAGONGL_ROOT}/src/server/server_log.c"
    "${DRAGONGL_ROOT}/src/server/server_metrics.c"
    "${DRAGONGL_ROOT}/src/server/server_tiles.c"
    "${DRAGONGL_ROOT}/src/server/server_traps.c"
    "${DRAGONGL_ROOT}/src/server/server_loot.c"
    "${DRAGONGL_ROOT}/src/server/player_stats.c"
    "${DRAGONGL_ROOT}/src/server/player_store.c"
    "${DRAGONGL_ROOT}/src/server/server_net.c"
    "${DRAGONGL_ROOT}/src/server/server_conn.c"
    "${DRAGONGL_ROOT}/src/server/auth.c"
    "${DRAGONGL_ROOT}/src/server/persist.c"
    "${DRAGONGL_ROOT}/src/server/writer.c"
    "${DRAGONGL_ROOT}/src/server/login_worker.c"
    "${DRAGONGL_ROOT}/src/server/ai.c"
    "${DRAGONGL_ROOT}/src/server/aoe.c"
    "${DRAGONGL_ROOT}/src/server/combat_log.c"
    "${DRAGONGL_ROOT}/src/server/pathfinding.c"
    "${DRAGONGL_ROOT}/src/server/server_combat.c"
    "${DRAGONGL_ROOT}/src/server/server_spawn.c"
    "${DRAGONGL_ROOT}/src/server/server_world.c"
    "${DRAGONGL_ROOT}/src/server/server_move.c"
    "${DRAGONGL_ROOT}/src/server/server_commands.c"
    "${DRAGONGL_ROOT}/src/server/cmd_character.c"
    "${DRAGONGL_ROOT}/src/server/cmd_social.c"
    "${DRAGONGL_ROOT}/src/server/cmd_shop.c"
    "${DRAGONGL_ROOT}/src/server/cmd_items.c"
    "${DRAGONGL_ROOT}/src/server/cmd_equip.c"
    "${DRAGONGL_ROOT}/src/server/cmd_ground.c"
    "${DRAGONGL_ROOT}/src/server/cmd_mine.c"
    "${DRAGONGL_ROOT}/src/server/inventory.c"
    "${DRAGONGL_ROOT}/src/server/npc_index.c"
    "${DRAGONGL_ROOT}/src/server/cmd_magic.c"
    "${DRAGONGL_ROOT}/src/server/cmd_world.c"
    "${DRAGONGL_ROOT}/src/server/cmd_dm.c"
    "${DRAGONGL_ROOT}/src/server/spell_cast.c"
    "${DRAGONGL_ROOT}/src/server/spell_router.c"
    "${DRAGONGL_ROOT}/src/server/tombstone.c"
    "${DRAGONGL_ROOT}/src/server/server_tls.c"
    "${DRAGONGL_ROOT}/src/server/server_options.c"
)
set(DRAGONGL_SERVER_SRCS "${DRAGONGL_ROOT}/src/server/main_server.c" ${DRAGONGL_SERVER_LIB_SRCS})

# --- Compiler flags -----------------------------------------------------------
# Optimization and debug information come from CMAKE_BUILD_TYPE (or from the
# distribution's flags: the RPM build passes %optflags), never from here.
#
# DRAGONGL_HARDENING (default ON) adds what a distribution build already
# provides: stack protector, _FORTIFY_SOURCE in optimized builds (unless the
# caller set it), PIE, full RELRO. Packagers turn it off so their own flags
# are the only ones (Fedora: _FORTIFY_SOURCE=3, which a forced =2 redefined).
option(DRAGONGL_HARDENING "Add hardening flags (off for distribution builds)" ON)

macro(dragongl_compile_settings)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    add_compile_definitions(_DEFAULT_SOURCE)
    add_compile_options(-Wall -Wextra -Wpedantic -Wformat -Wformat-security -Wshadow -Wconversion)
    if(DRAGONGL_HARDENING)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        include(CheckPIESupported)
        check_pie_supported()
        add_compile_options(-fstack-protector-strong)
        if(NOT CMAKE_C_FLAGS MATCHES "_FORTIFY_SOURCE")
            # Needs optimization: inert (and warned about) at -O0.
            add_compile_options($<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
        endif()
        add_link_options(-Wl,-z,relro,-z,now)
    endif()
endmacro()

# --- Generated header ----------------------------------------------------------
# <dir>/dragongl_config.h: DRAGONGL_VERSION and DRAGONGL_PKGDATADIR (where the
# package installs shaders, game data and tools).
function(dragongl_generate_config dir)
    include(GNUInstallDirs)
    set(DRAGONGL_PKGDATADIR "${CMAKE_INSTALL_FULL_DATADIR}/dragongl")
    configure_file("${DRAGONGL_ROOT}/cmake/dragongl_config.h.in" "${dir}/dragongl_config.h" @ONLY)
endfunction()
