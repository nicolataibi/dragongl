#!/usr/bin/env bash
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# quality.sh — the quality checks of CI, runnable locally.
#
# Usage: tools/quality.sh <check>... (run tools/quality.sh -h for the list)
#
# Every check exits non-zero on a failure. Build trees go to build-quality/
# (git-ignored); the clang tools must be version 18, the version CI pins
# (Ubuntu 24.04): another version formats and diagnoses differently.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly ROOT
readonly OUT="$ROOT/build-quality"
readonly LIMITS="$ROOT/tools/quality_limits.json"
readonly LLVM_VERSION=18
JOBS="$(nproc 2>/dev/null || echo 4)"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    C_BOLD=$'\e[1m'; C_DIM=$'\e[2m'; C_RED=$'\e[31m'; C_GREEN=$'\e[32m'; C_BLUE=$'\e[34m'
    C_RESET=$'\e[0m'
else
    C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_BLUE=""; C_RESET=""
fi
section() { printf '\n%s==> %s%s\n' "$C_BOLD$C_BLUE" "$*" "$C_RESET"; }
info()    { printf '%s  -> %s%s\n' "$C_DIM" "$*" "$C_RESET"; }
ok()      { printf '%s  ok:%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
die()     { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
${C_BOLD}quality.sh${C_RESET} — the quality checks of CI, runnable locally

Usage: tools/quality.sh <check>...

Checks
  format          clang-format --dry-run: every C file is in the style of .clang-format
  format-fix      reformat every C file in place
  tidy            clang-tidy (.clang-tidy) on the main project, the unit
                  tests, the integration tests and the fuzz targets
  data            tools/check_data.py: schema, references, canonical form of data/
  metrics         tools/metrics.py: function and file length, coverage, against
                  the limits of tools/quality_limits.json
  coverage        unit + integration suites with llvm-cov instrumentation;
                  report in build-quality/coverage/, fails under the
                  coverage_line_percent of tools/quality_limits.json
  tsan            integration suite (in-process and network) under ThreadSanitizer
  fuzz [SECONDS]  build the libFuzzer targets of tests/fuzz, write their seeds
                  and run each for SECONDS (default 60); crashes are saved
                  in build-quality/fuzz/artifacts/
  all             format tidy data coverage metrics tsan, then fuzz 30

Options
  -j N            parallel jobs (default: number of CPUs)
  -h, --help      this help

Environment
  CC              compiler for coverage/tsan/fuzz (default: clang; must be clang)
  NO_COLOR=1      disable colored output
EOF
}

# --- Tools ----------------------------------------------------------------------------------
# The versioned name first (Debian/Ubuntu), then the plain one if it is v18.
llvm_tool() {
    local name=$1 cand
    for cand in "$name-$LLVM_VERSION" "$name"; do
        if command -v "$cand" >/dev/null 2>&1; then
            if [[ $cand == "$name" ]] &&
                ! "$cand" --version 2>/dev/null | grep -q "version $LLVM_VERSION\."; then
                continue
            fi
            echo "$cand"
            return 0
        fi
    done
    die "$name $LLVM_VERSION not found (apt install clang-tools-$LLVM_VERSION / llvm-$LLVM_VERSION)"
}

need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found"; }

c_sources() {
    git -C "$ROOT" ls-files --cached --others --exclude-standard '*.c' '*.h' |
        grep -v '^src/shared/cJSON\.'
}

# configure <dir> <source dir> [cmake args...]
configure() {
    local dir=$1 src=$2
    shift 2
    local gen=()
    if command -v ninja >/dev/null 2>&1; then gen=(-G Ninja); fi
    cmake -S "$src" -B "$dir" "${gen[@]}" -DCMAKE_C_COMPILER="${CC:-clang}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" >/dev/null
    cmake --build "$dir" --parallel "$JOBS" >/dev/null
}

limit() {
    python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$LIMITS" "$1"
}

# --- Checks ---------------------------------------------------------------------------------
check_format() {
    section "clang-format ($([[ ${1:-} == fix ]] && echo fix || echo check))"
    local cf
    cf="$(llvm_tool clang-format)"
    cd "$ROOT"
    if [[ ${1:-} == fix ]]; then
        c_sources | xargs -r "$cf" -i
        ok "$(c_sources | wc -l) files formatted"
    else
        c_sources | xargs -r "$cf" --dry-run --Werror
        ok "$(c_sources | wc -l) files in the style of .clang-format"
    fi
}

check_tidy() {
    section "clang-tidy"
    local rct
    rct="$(llvm_tool run-clang-tidy)"
    llvm_tool clang-tidy >/dev/null
    local d="$OUT/tidy"
    info "configure main, unit, integration and fuzz trees (compile_commands.json)"
    configure "$d/main" "$ROOT" -DCMAKE_BUILD_TYPE=Debug
    configure "$d/unit" "$ROOT/tests" -DCMAKE_BUILD_TYPE=Debug
    configure "$d/integration" "$ROOT/tests/integration" -DCMAKE_BUILD_TYPE=Debug
    configure "$d/fuzz" "$ROOT/tests/fuzz"
    # One clang-tidy process per file (run-clang-tidy): the static analyzer
    # keeps state across the files of a single process.
    local -a tidy=("$rct" -quiet -j "$JOBS" -clang-tidy-binary "$(llvm_tool clang-tidy)")
    "${tidy[@]}" -p "$d/main" "$ROOT/src/(server|client|shared)/(?!cJSON).*\.c$"
    "${tidy[@]}" -p "$d/unit" "$ROOT/tests/[^/]*\.c$"
    "${tidy[@]}" -p "$d/integration" "$ROOT/tests/integration/.*\.c$"
    "${tidy[@]}" -p "$d/fuzz" "$ROOT/tests/fuzz/.*\.c$"
    ok "no clang-tidy finding"
}

check_data() {
    section "game data"
    python3 "$ROOT/tools/check_data.py"
}

check_metrics() {
    section "metrics"
    mkdir -p "$OUT"
    python3 "$ROOT/tools/metrics.py" --json "$OUT/metrics.json"
}

check_coverage() {
    section "coverage (llvm-cov)"
    local profdata cov
    profdata="$(llvm_tool llvm-profdata)"
    cov="$(llvm_tool llvm-cov)"
    local d="$OUT/coverage" flags="-fprofile-instr-generate -fcoverage-mapping"
    rm -rf "$d/profiles"
    mkdir -p "$d/profiles"
    info "build the unit and integration suites with instrumentation"
    configure "$d/unit" "$ROOT/tests" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="$flags" \
        -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
    configure "$d/integration" "$ROOT/tests/integration" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
    info "run both suites (every process writes its own profile)"
    # %p: one file per process, the network tests start many servers.
    export LLVM_PROFILE_FILE="$d/profiles/%p-%m.profraw"
    ctest --test-dir "$d/unit" -j "$JOBS" --output-on-failure >/dev/null
    ctest --test-dir "$d/integration" -j "$JOBS" --output-on-failure >/dev/null
    unset LLVM_PROFILE_FILE
    "$profdata" merge -sparse "$d"/profiles/*.profraw -o "$d/all.profdata"
    local -a objects=("$d/integration/dragongl_server_it" -object "$d/integration/server_harness")
    local t
    for t in "$d"/unit/test_*; do
        [[ -x $t && -f $t ]] && objects+=(-object "$t")
    done
    # Measured: the server and the shared code (the client is covered by
    # the smoke test, which runs on a GPU-less runner without llvm-cov).
    local -a filter=(-ignore-filename-regex='(cJSON|/tests/|/generated/|/src/client/|/include/)')
    # main_server.c is built twice (with main() for the server, without it
    # for the harness): llvm-cov notes the functions that differ; harmless.
    exec 3>&2 2> >(grep -v 'functions have mismatched data' >&3)
    "$cov" report "${objects[@]}" -instr-profile="$d/all.profdata" "${filter[@]}" \
        >"$d/report.txt"
    "$cov" show "${objects[@]}" -instr-profile="$d/all.profdata" "${filter[@]}" \
        -format=html -output-dir="$d/html" >/dev/null
    "$cov" export "${objects[@]}" -instr-profile="$d/all.profdata" "${filter[@]}" \
        -summary-only >"$d/export.json"
    exec 2>&3 3>&-
    python3 - "$d/export.json" "$d/summary.json" <<'EOF'
import json, sys
totals = json.load(open(sys.argv[1]))["data"][0]["totals"]
summary = {k: round(totals[k]["percent"], 2) for k in ("lines", "functions", "regions", "branches")}
summary["line_percent"] = summary.pop("lines")
json.dump(summary, open(sys.argv[2], "w"), indent=2)
print("  lines %.2f%%  functions %.2f%%  regions %.2f%%  branches %.2f%%" % (
    summary["line_percent"], summary["functions"], summary["regions"], summary["branches"]))
EOF
    info "report: ${d#"$ROOT"/}/report.txt, html: ${d#"$ROOT"/}/html/index.html"
    local min line
    min="$(limit coverage_line_percent)"
    line="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["line_percent"])' \
        "$d/summary.json")"
    if python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) >= float(sys.argv[2]) else 1)' \
        "$line" "$min"; then
        ok "line coverage $line% >= $min%"
    else
        die "line coverage $line% is under the limit $min% (tools/quality_limits.json)"
    fi
}

check_tsan() {
    section "ThreadSanitizer (integration suite)"
    local d="$OUT/tsan"
    configure "$d" "$ROOT/tests/integration" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1" \
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
    # A report fails the in-process tests through the exit code and the
    # network tests through the server log (test_server.py).
    TSAN_OPTIONS="second_deadlock_stack=1 exitcode=66 ${TSAN_OPTIONS:-}" \
        ctest --test-dir "$d" -j "$JOBS" --output-on-failure
    ok "no data race reported"
}

check_fuzz() {
    local seconds=${1:-60}
    section "fuzzing (libFuzzer, ${seconds}s per target)"
    local d="$OUT/fuzz" t
    configure "$d/build" "$ROOT/tests/fuzz"
    mkdir -p "$d/seeds" "$d/corpus" "$d/artifacts" "$d/logs"
    # Saves written by the targets go to a private directory: tmpfs if any.
    local tmp="${TMPDIR:-/tmp}"
    [[ -d /dev/shm && -w /dev/shm ]] && tmp=/dev/shm
    for t in fuzz_wire fuzz_persist fuzz_text_cmd; do
        rm -rf "${d:?}/seeds/$t"
        DGL_FUZZ_SEEDS="$d/seeds/$t" TMPDIR="$tmp" "$d/build/$t" >/dev/null 2>&1 ||
            die "$t: cannot write its seeds"
        mkdir -p "$d/corpus/$t"
        info "$t: $(find "$d/seeds/$t" -type f | wc -l) seeds, corpus ${d#"$ROOT"/}/corpus/$t"
        if ! TMPDIR="$tmp" "$d/build/$t" -max_total_time="$seconds" -max_len=8192 -timeout=10 \
            -rss_limit_mb=4096 -print_final_stats=1 -artifact_prefix="$d/artifacts/$t-" \
            "$d/corpus/$t" "$d/seeds/$t" >"$d/logs/$t.log" 2>&1; then
            tail -n 60 "$d/logs/$t.log" >&2
            die "$t: crash, artifact in ${d#"$ROOT"/}/artifacts/ (replay: $d/build/$t <file>)"
        fi
        ok "$t: $(sed -n 's/^stat::number_of_executed_units: *//p' "$d/logs/$t.log") runs"
    done
}

# --- Main -----------------------------------------------------------------------------------
main() {
    local -a checks=()
    while (($#)); do
        case $1 in
        -h | --help) usage; exit 0 ;;
        -j) [[ $# -ge 2 ]] || die "-j needs a number"; JOBS=$2; shift ;;
        -j*) JOBS=${1#-j} ;;
        fuzz)
            if [[ ${2:-} =~ ^[0-9]+$ ]]; then checks+=("fuzz:$2"); shift; else checks+=(fuzz:60); fi
            ;;
        all) checks+=(format tidy data coverage metrics tsan fuzz:30) ;;
        format | format-fix | tidy | data | metrics | coverage | tsan) checks+=("$1") ;;
        *) usage >&2; die "unknown check '$1'" ;;
        esac
        shift
    done
    ((${#checks[@]})) || { usage; exit 2; }
    need cmake
    need python3
    local c
    for c in "${checks[@]}"; do
        case $c in
        format) check_format ;;
        format-fix) check_format fix ;;
        tidy) check_tidy ;;
        data) check_data ;;
        metrics) check_metrics ;;
        coverage) check_coverage ;;
        tsan) check_tsan ;;
        fuzz:*) check_fuzz "${c#fuzz:}" ;;
        esac
    done
    printf '\n%squality.sh: %s passed%s\n' "$C_GREEN$C_BOLD" "${checks[*]}" "$C_RESET"
}

main "$@"
