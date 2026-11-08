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
    add_compile_options(-Wall -Wextra -Wpedantic -Wformat -Wformat-security)
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
