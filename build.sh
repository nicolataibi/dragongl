#!/usr/bin/env bash
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# build.sh — quick dev loop for Dragon GL
#
# Usage:
#   ./build.sh            # Release build + full test suite (default)
#   ./build.sh -d         # Debug build + full test suite (ASan/UBSan)
#   ./build.sh -r         # RelWithDebInfo (profiling)
#   ./build.sh -s         # MinSizeRel
#   ./build.sh -t         # tests only (no rebuild)
#   ./build.sh -c         # clean build (rm -rf build dirs) + rebuild
#   ./build.sh -n         # build only, skip the test suite
#   ./build.sh -h         # help
#
# Options can be combined, e.g. ./build.sh -c -n  (or -cn).
# Run ./build.sh -h for the complete list.

set -Eeuo pipefail

readonly SCRIPT_NAME="${0##*/}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT

# --- Build directories (all git-ignored) ------------------------------------
# build/ is the directory the dragongl-server / dragongl-client launchers use.
readonly BUILD_DIR="$ROOT/build"
readonly UNIT_DIR="$ROOT/build-tests"
readonly UNIT_ASAN_DIR="$ROOT/build-tests-asan"
readonly INTEG_DIR="$ROOT/build-integration"
readonly STAGE_DIR="$ROOT/build/stage"

readonly SANITIZE_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
readonly SANITIZE_LDFLAGS="-fsanitize=address,undefined"

# --- Defaults -------------------------------------------------------------------
BUILD_TYPE="Release"
TYPE_SET=""          # option that chose the build type (conflict detection)
SANITIZE=0
CLEAN=0
DO_BUILD=1
DO_TESTS=1
RUN_UNIT=1
RUN_INTEG=1
DO_ANALYZE=0
DO_STAGE=0
DO_CLIENT_TEST=0
SERVER_ONLY=0
WERROR=0
USE_GCC=0
VERBOSE=0
JOBS=""

# --- Output helpers ---------------------------------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    C_BOLD=$'\e[1m'; C_DIM=$'\e[2m'; C_RED=$'\e[31m'; C_GREEN=$'\e[32m'
    C_YELLOW=$'\e[33m'; C_BLUE=$'\e[34m'; C_RESET=$'\e[0m'
else
    C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_RESET=""
fi

section() { printf '\n%s==> %s%s\n' "$C_BOLD$C_BLUE" "$*" "$C_RESET"; }
info()    { printf '%s  -> %s%s\n' "$C_DIM" "$*" "$C_RESET"; }
warn()    { printf '%swarning:%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()     { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

SUMMARY=()
CURRENT_STEP=""
record() { SUMMARY+=("$(printf '%-28s %s' "$1" "$2")"); }

on_error() {
    local status=$? line=$1
    if [[ -n $CURRENT_STEP ]]; then
        record "$CURRENT_STEP" "${C_RED}FAILED${C_RESET}"
        printf '\n%s%s failed: step "%s" (exit %d)%s\n' "$C_RED$C_BOLD" "$SCRIPT_NAME" \
            "$CURRENT_STEP" "$status" "$C_RESET" >&2
    else
        printf '\n%s%s failed (exit %d, line %d)%s\n' "$C_RED$C_BOLD" "$SCRIPT_NAME" \
            "$status" "$line" "$C_RESET" >&2
    fi
    print_summary >&2
    exit "$status"
}
trap 'on_error $LINENO' ERR

print_summary() {
    ((${#SUMMARY[@]})) || return 0
    printf '\n%sSummary%s\n' "$C_BOLD" "$C_RESET"
    local entry
    for entry in "${SUMMARY[@]}"; do printf '  %s\n' "$entry"; done
}

# run_timed <label> <command...> — runs a step, records its duration
run_timed() {
    local label=$1; shift
    local start=$SECONDS
    CURRENT_STEP=$label
    if ((VERBOSE)); then printf '%s  $ %s%s\n' "$C_DIM" "$*" "$C_RESET"; fi
    "$@"
    CURRENT_STEP=""
    record "$label" "${C_GREEN}ok${C_RESET} ($((SECONDS - start)) s)"
}

usage() {
    cat <<EOF
${C_BOLD}$SCRIPT_NAME${C_RESET} — quick dev loop for Dragon GL

${C_BOLD}Usage:${C_RESET} ./$SCRIPT_NAME [options]

${C_BOLD}Build type${C_RESET} (pick one; default: Release)
  -d, --debug          Debug + ASan/UBSan (build and tests)
  -r, --relwithdebinfo RelWithDebInfo (profiling)
  -s, --minsizerel     MinSizeRel

${C_BOLD}What to run${C_RESET} (default: build + unit tests + integration tests)
  -t, --tests-only     run the tests of the last build, without rebuilding
                       (add -d for the ASan unit build in build-tests-asan/)
  -n, --no-tests       build only, skip the test suite
  -u, --unit-only      run only the unit suite (tests/)
  -i, --integ-only     run only the server integration suite (tests/integration)
  -S, --server-only    build only the server (configured without the client:
                       no Vulkan, GLFW or glslc needed)
  -a, --analyze        also run cppcheck (as in CI)
  -x, --stage          also install into build/stage to check the install layout
  -k, --client-test    also run the client smoke test: real server + real client
                       under Xvfb, Mesa lavapipe and the Vulkan validation layer
                       (needs Xvfb, mesa-vulkan-drivers, the validation layer
                       and python3-xlib)

${C_BOLD}Build options${C_RESET}
  -c, --clean          remove every build directory first, then rebuild
  -w, --werror         treat compiler warnings as errors (as in CI)
  -g, --gcc            use gcc instead of clang
  -j, --jobs N         parallel jobs (default: number of CPUs)
  -v, --verbose        echo the commands and show the full compiler output
  -h, --help           this help

Options can be combined, e.g. ${C_BOLD}./$SCRIPT_NAME -c -n${C_RESET} (or ${C_BOLD}-cn${C_RESET}),
${C_BOLD}./$SCRIPT_NAME -dwa${C_RESET}, ${C_BOLD}./$SCRIPT_NAME -j8 -i${C_RESET}.

${C_BOLD}Directories${C_RESET}
  build/               main project (used by ./dragongl-server and ./dragongl-client)
  build-tests/         unit suite, Release   (build-tests-asan/ with -d)
  build-integration/   server integration suite
  build/stage/         install tree (-x)

${C_BOLD}Environment${C_RESET}
  NO_COLOR=1           disable colored output
  CMAKE_GENERATOR      generator for new build directories (default: Ninja if available)
EOF
}

# --- Argument parsing ---------------------------------------------------------
set_type() {
    local type=$1 opt=$2
    if [[ -n $TYPE_SET && $TYPE_SET != "$opt" ]]; then
        die "$opt conflicts with $TYPE_SET: choose one build type"
    fi
    BUILD_TYPE=$type
    TYPE_SET=$opt
}

set_jobs() {
    [[ $1 =~ ^[1-9][0-9]*$ ]] || die "-j expects a positive number, got '$1'"
    JOBS=$1
}

handle_short() {
    case $1 in
        d) set_type Debug -d; SANITIZE=1 ;;
        r) set_type RelWithDebInfo -r ;;
        s) set_type MinSizeRel -s ;;
        t) DO_BUILD=0 ;;
        n) DO_TESTS=0 ;;
        u) RUN_INTEG=0 ;;
        i) RUN_UNIT=0 ;;
        S) SERVER_ONLY=1 ;;
        a) DO_ANALYZE=1 ;;
        x) DO_STAGE=1 ;;
        k) DO_CLIENT_TEST=1 ;;
        c) CLEAN=1 ;;
        w) WERROR=1 ;;
        g) USE_GCC=1 ;;
        v) VERBOSE=1 ;;
        h) usage; exit 0 ;;
        *) die "unknown option -$1 (see ./$SCRIPT_NAME -h)" ;;
    esac
}

parse_args() {
    while (($#)); do
        case $1 in
            --debug)          handle_short d ;;
            --relwithdebinfo) handle_short r ;;
            --minsizerel)     handle_short s ;;
            --tests-only)     handle_short t ;;
            --no-tests)       handle_short n ;;
            --unit-only)      handle_short u ;;
            --integ-only)     handle_short i ;;
            --server-only)    handle_short S ;;
            --analyze)        handle_short a ;;
            --stage)          handle_short x ;;
            --client-test)    handle_short k ;;
            --clean)          handle_short c ;;
            --werror)         handle_short w ;;
            --gcc)            handle_short g ;;
            --verbose)        handle_short v ;;
            --help)           handle_short h ;;
            --jobs)           (($# > 1)) || die "--jobs needs a value"; set_jobs "$2"; shift ;;
            --jobs=*)         set_jobs "${1#*=}" ;;
            --)               shift; (($#)) && die "unexpected argument '$1'"; break ;;
            -?*)
                # Combined short options: -cn, -dwa, -j8, -cj 8
                local flags=${1#-} k ch
                for ((k = 0; k < ${#flags}; k++)); do
                    ch=${flags:k:1}
                    if [[ $ch == j ]]; then
                        if ((k + 1 < ${#flags})); then
                            set_jobs "${flags:k+1}"
                        else
                            (($# > 1)) || die "-j needs a value"
                            set_jobs "$2"; shift
                        fi
                        break
                    fi
                    handle_short "$ch"
                done
                ;;
            *) die "unexpected argument '$1' (see ./$SCRIPT_NAME -h)" ;;
        esac
        shift
    done

    if ((!DO_BUILD && CLEAN)); then die "-t (tests only) cannot be combined with -c (clean)"; fi
    if ((!DO_BUILD && !DO_TESTS)); then die "-t and -n together leave nothing to do"; fi
    if ((!RUN_UNIT && !RUN_INTEG)); then die "-u and -i are exclusive: use one, or neither for both"; fi
    if ((!DO_TESTS && (!RUN_UNIT || !RUN_INTEG))); then
        warn "-u/-i have no effect together with -n"
    fi
    if ((SERVER_ONLY && DO_STAGE)); then die "-S and -x cannot be combined: the install needs every target"; fi
    if ((SERVER_ONLY && DO_CLIENT_TEST)); then die "-S and -k cannot be combined: the client test needs the client"; fi
    if ((!DO_BUILD && (DO_STAGE || SERVER_ONLY || WERROR || USE_GCC))); then
        warn "-x/-S/-w/-g only affect builds and are ignored with -t"
    fi
    [[ -n $JOBS ]] || JOBS=$(nproc 2>/dev/null || echo 4)
}

# --- Environment checks --------------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found: $2"; }

check_tools() {
    need cmake "install CMake >= 3.16"
    if ((DO_BUILD)); then
        if ((USE_GCC)); then need gcc "install gcc, or drop -g"
        else need clang "install clang, or use -g for gcc"; fi
        ((SERVER_ONLY)) || need glslc "install glslc (shaderc) to compile the Vulkan shaders"
    fi
    if ((DO_TESTS && RUN_INTEG)); then need python3 "the integration suite needs Python 3"; fi
    if ((DO_ANALYZE)); then need cppcheck "install cppcheck, or drop -a"; fi
    if ((DO_CLIENT_TEST)); then
        need python3 "the client test needs Python 3"
        need Xvfb "install Xvfb (xorg-x11-server-Xvfb / xvfb), or drop -k"
    fi
}

generator_for() {
    # Reuse the generator of an existing build tree: CMake refuses to switch.
    local dir=$1 cached
    if [[ -f $dir/CMakeCache.txt ]]; then
        cached=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$dir/CMakeCache.txt")
        if [[ -n $cached ]]; then printf '%s' "$cached"; return; fi
    fi
    if [[ -n ${CMAKE_GENERATOR:-} ]]; then printf '%s' "$CMAKE_GENERATOR"
    elif command -v ninja >/dev/null 2>&1; then printf 'Ninja'
    else printf 'Unix Makefiles'; fi
}

# CMake cannot switch compilers inside a build tree: it deletes the cache
# and re-configures WITHOUT the -D values of that command line, silently
# dropping -Werror and the sanitizer flags. Reset the cache ourselves first.
reset_cache_if_compiler_changed() {
    local dir=$1 want=$2 cached have
    [[ -f $dir/CMakeCache.txt ]] || return 0
    cached=$(sed -n 's/^CMAKE_C_COMPILER:[A-Z]*=//p' "$dir/CMakeCache.txt")
    [[ -n $cached ]] || return 0
    have=$(readlink -f -- "$(command -v -- "$cached" 2>/dev/null || printf '%s' "$cached")")
    want=$(readlink -f -- "$(command -v -- "$want")")
    if [[ $have != "$want" ]]; then
        info "compiler changed (${have##*/} -> ${want##*/}): resetting the CMake cache of ${dir#"$ROOT"/}"
        rm -rf -- "$dir/CMakeCache.txt" "$dir/CMakeFiles"
    fi
}

# configure <dir> <source dir> [extra cmake args...]
# Every cache variable that depends on the options is passed explicitly, so
# switching between -d and a Release build in the same directory never keeps
# stale sanitizer or -Werror flags.
configure() {
    local dir=$1 src=$2; shift 2
    local cflags="" ldflags=""
    if ((SANITIZE)); then cflags=$SANITIZE_FLAGS; ldflags=$SANITIZE_LDFLAGS; fi
    if ((WERROR)); then cflags="${cflags:+$cflags }-Werror"; fi
    local compiler=clang
    if ((USE_GCC)); then compiler=gcc; fi
    reset_cache_if_compiler_changed "$dir" "$compiler"
    local args=(-S "$src" -B "$dir" -G "$(generator_for "$dir")"
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
                -DCMAKE_C_FLAGS="$cflags"
                -DCMAKE_EXE_LINKER_FLAGS="$ldflags")
    if ((USE_GCC)); then args+=(-DCMAKE_C_COMPILER=gcc -DDRAGONGL_USE_CLANG=OFF)
    else args+=(-DCMAKE_C_COMPILER=clang -DDRAGONGL_USE_CLANG=ON); fi
    args+=("$@")
    if ((VERBOSE)); then
        cmake "${args[@]}"
    elif ! cmake "${args[@]}" >"$dir/configure.log" 2>&1; then
        cat "$dir/configure.log" >&2
        if ((SANITIZE)) && ! ((USE_GCC)); then
            warn "sanitizer builds with clang need compiler-rt (Fedora: compiler-rt," \
                 "Debian/Ubuntu: libclang-rt-dev); -g builds with gcc instead"
        fi
        return 1
    fi
    rm -f "$dir/configure.log"
}

build_dir() {
    local dir=$1; shift
    local args=(--build "$dir" --parallel "$JOBS" "$@")
    if ((VERBOSE)); then args+=(--verbose); fi
    cmake "${args[@]}"
}

# --- Sections ------------------------------------------------------------------
step_clean() {
    section "Clean"
    local d
    for d in "$BUILD_DIR" "$UNIT_DIR" "$UNIT_ASAN_DIR" "$INTEG_DIR"; do
        if [[ -e $d ]]; then info "rm -rf ${d#"$ROOT"/}"; rm -rf -- "$d"; fi
    done
    record "clean" "${C_GREEN}ok${C_RESET}"
}

step_build_main() {
    local what=$BUILD_TYPE
    if ((SANITIZE)); then what+=" + ASan/UBSan"; fi
    if ((SERVER_ONLY)); then what+=", server only"; fi
    section "Main project — $what"
    mkdir -p "$BUILD_DIR"
    # -S configures without the client: no Vulkan, GLFW or glslc needed.
    local client=ON
    if ((SERVER_ONLY)); then client=OFF; fi
    run_timed "configure main" configure "$BUILD_DIR" "$ROOT" -DDRAGONGL_BUILD_CLIENT=$client
    run_timed "build main" build_dir "$BUILD_DIR"
}

install_stage() {
    # The configured prefix, not --prefix: the binaries, the unit and the
    # man pages carry the paths fixed at configure time.
    rm -rf -- "$STAGE_DIR"
    if ((VERBOSE)); then
        DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"
    else
        DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR" >/dev/null
    fi
}

step_stage() {
    local prefix
    prefix=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
    section "Install layout (build/stage, prefix ${prefix:-?})"
    run_timed "install to stage" install_stage
    info "$(find "$STAGE_DIR" -type f | wc -l) files under ${STAGE_DIR#"$ROOT"/}${prefix}"
}

unit_dir() { if ((SANITIZE)); then printf '%s' "$UNIT_ASAN_DIR"; else printf '%s' "$UNIT_DIR"; fi; }

step_build_unit() {
    local dir; dir=$(unit_dir)
    section "Unit tests — build (${dir#"$ROOT"/})"
    mkdir -p "$dir"
    run_timed "configure unit" configure "$dir" "$ROOT/tests"
    run_timed "build unit" build_dir "$dir"
}

step_build_integ() {
    section "Integration tests — build (${INTEG_DIR#"$ROOT"/})"
    mkdir -p "$INTEG_DIR"
    run_timed "configure integration" configure "$INTEG_DIR" "$ROOT/tests/integration"
    run_timed "build integration" build_dir "$INTEG_DIR"
}

run_ctest() {
    local dir=$1; shift
    [[ -f $dir/CTestTestfile.cmake ]] ||
        die "${dir#"$ROOT"/} is not built: run ./$SCRIPT_NAME without -t first"
    local args=(--test-dir "$dir" --output-on-failure "$@")
    if ((VERBOSE)); then args+=(--verbose); fi
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" ctest "${args[@]}"
}

step_test_unit() {
    section "Unit tests — run"
    run_timed "unit tests" run_ctest "$(unit_dir)" --parallel "$JOBS"
}

step_test_integ() {
    section "Integration tests — run (the first run generates a world: ~1-2 min)"
    run_timed "integration tests" run_ctest "$INTEG_DIR"
}

step_client_test() {
    section "Client smoke test (Xvfb + lavapipe + validation layer)"
    [[ -x $BUILD_DIR/dragongl_client ]] ||
        die "build/dragongl_client is not built: run ./$SCRIPT_NAME without -t first"
    run_timed "client test" env DRAGONGL_CLIENT_TEST_REQUIRE=1 \
        DRAGONGL_CLIENT="$BUILD_DIR/dragongl_client" DRAGONGL_SERVER="$BUILD_DIR/dragongl_server" \
        ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        python3 "$ROOT/tests/client/test_client.py"
}

step_analyze() {
    section "Static analysis (cppcheck)"
    run_timed "cppcheck" cppcheck -q -j "$JOBS" --enable=warning,portability \
        --inline-suppr --error-exitcode=1 --suppress=missingIncludeSystem \
        -D_DEFAULT_SOURCE -DDRAGONGL_VERSION='"0"' -DDRAGONGL_PKGDATADIR='"/usr/share/dragongl"' \
        -I"$ROOT/include" -I"$ROOT/src/server" -I"$ROOT/src/client" \
        -i "$ROOT/src/shared/cJSON.c" "$ROOT/src"
}

# --- Main -------------------------------------------------------------------------
main() {
    parse_args "$@"
    cd -- "$ROOT"
    check_tools

    local mode="build + tests"
    ((DO_BUILD)) || mode="tests only"
    ((DO_TESTS)) || mode="build only"
    local compiler=clang
    if ((USE_GCC)); then compiler=gcc; fi
    if ((WERROR)); then compiler+=" -Werror"; fi
    printf '%sDragon GL%s — %s, %s, %s, %s jobs\n' "$C_BOLD" "$C_RESET" \
        "$BUILD_TYPE" "$mode" "$compiler" "$JOBS"

    local start=$SECONDS
    if ((CLEAN)); then step_clean; fi

    if ((DO_BUILD)); then
        step_build_main
        if ((DO_STAGE)); then step_stage; fi
        if ((DO_TESTS && RUN_UNIT)); then step_build_unit; fi
        if ((DO_TESTS && RUN_INTEG)); then step_build_integ; fi
    fi

    if ((DO_TESTS && RUN_UNIT)); then step_test_unit; fi
    if ((DO_TESTS && RUN_INTEG)); then step_test_integ; fi
    if ((DO_CLIENT_TEST)); then step_client_test; fi
    if ((DO_ANALYZE)); then step_analyze; fi

    print_summary
    printf '\n%sDone%s in %d s.\n' "$C_GREEN$C_BOLD" "$C_RESET" "$((SECONDS - start))"
}

main "$@"
