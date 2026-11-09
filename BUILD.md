# Dragon GL — Build Instructions

This document provides instructions for compiling the Dragon GL client, server, shaders, and test suite from source.

---

## 1. Toolchain & Dependencies

Dragon GL is written in standard **C11** and is designed to build warning-clean on modern Linux distributions.

### Required Tools & Libraries:
- **C Compiler:** `clang` (default and recommended) or `gcc` with C11 support
- **Build System:** `cmake` (version 3.16 or higher) and `make` (or `ninja`)
- **Shader Compiler:** `glslc` (from Google Shaderc / Vulkan SDK) for compiling GLSL to SPIR-V
- **Graphics & Windowing:**
  - `glfw3` (GLFW development libraries: the window and its Vulkan surface)
  - `Vulkan` loader and header files (Vulkan 1.3)
- **System Libraries:** POSIX Threads (`pthread`), Math library (`libm`), PkgConfig (`pkg-config` / `pkgconf`)
- **Cryptography:** OpenSSL 3 (TLS 1.3 between client and server); `libsodium` for the server (Argon2id password hashing, constant-time comparisons)
- **Python:** Python 3 (for build verification and developer balance tools)

### Installing Dependencies on Fedora:
```bash
sudo dnf install -y \
    cmake \
    clang \
    glslc \
    pkgconf-pkg-config \
    glfw-devel \
    vulkan-loader-devel \
    libsodium-devel \
    openssl-devel \
    python3
```

### Installing Dependencies on Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    clang \
    glslc \
    pkg-config \
    libglfw3-dev \
    libvulkan-dev \
    libsodium-dev \
    libssl-dev \
    python3
```

---

## 2. Quick Dev Loop: `build.sh`

`build.sh` in the repository root wraps every step below: main project, unit suite, server integration suite, cppcheck and an install check. Without options it builds in Release and runs both test suites.

```bash
./build.sh            # Release build + full test suite (default)
./build.sh -d         # Debug build + full test suite (ASan/UBSan)
./build.sh -r         # RelWithDebInfo (profiling)
./build.sh -s         # MinSizeRel
./build.sh -t         # tests only (no rebuild)
./build.sh -c         # clean build (rm -rf build dirs) + rebuild
./build.sh -n         # build only, skip the test suite
./build.sh -h         # help
```

| Option | Effect |
| --- | --- |
| `-u` / `-i` | run only the unit suite / only the integration suite |
| `-S` | server only: configures with `-DDRAGONGL_BUILD_CLIENT=OFF`, so neither Vulkan, GLFW nor glslc is needed |
| `-a` | also run cppcheck, with the same options as CI |
| `-x` | also install into `build/stage` (`DESTDIR`, with the configured prefix: `/usr/local` unless `CMAKE_INSTALL_PREFIX` is set) to check the install layout |
| `-k` | also run the client smoke test (`tests/client/test_client.py`): a real server and the real client under Xvfb with Mesa lavapipe and the Vulkan validation layer — login, rendering, window resizes, Escape, server shutdown; any validation message fails. Needs Xvfb, `mesa-vulkan-drivers` (Fedora: `mesa-vulkan-drivers`), the validation layer (`vulkan-validation-layers` / `vulkan-validationlayers`) and `python3-xlib` |
| `-w` | warnings are errors (as in CI) |
| `-g` | gcc instead of clang |
| `-j N` | parallel jobs (default: number of CPUs) |
| `-v` | echo the commands and show the full output |

Options combine: `./build.sh -cn`, `./build.sh -dwa`, `./build.sh -j8 -i`. Conflicting options (two build types, `-t` with `-c`) are rejected. Every option that changes the compiler flags is passed to CMake on each run, so switching between `-d` and a Release build in the same directory never keeps stale sanitizer flags. The script prints a summary with the duration of each step and exits with a non-zero status on the first failure.

Sanitizer builds (`-d`) with clang need compiler-rt (Fedora: `compiler-rt`, Debian/Ubuntu: `libclang-rt-dev`); `-g` uses gcc instead. Release builds do not need it.

Build directories: `build/` (used by `./dragongl-server` and `./dragongl-client`), `build-tests/` (`build-tests-asan/` with `-d`), `build-integration/`, and `build-quality/` for `tools/quality.sh` (removed by `-c` too).

The quality gates of CI (clang-format, clang-tidy, game data, metrics, coverage, ThreadSanitizer, fuzzing) are in `tools/quality.sh`, described in `DEVELOPMENT.md`; they need clang 18 and its tools (Fedora: `clang-tools-extra llvm compiler-rt python3-jsonschema`; Debian/Ubuntu: `clang-format-18 clang-tidy-18 llvm-18 libclang-rt-18-dev python3-jsonschema`).

The sections below describe the same steps done by hand.

---

## 3. Manual Build Workflow

### Step 1: Configure
Generate build files using an out-of-source directory:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**Build Configuration Options:**

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Release`, `RelWithDebInfo`, `MinSizeRel` or `Debug`. The optimisation level is CMake's (`-O3` for Release); `CMAKE_C_FLAGS` from the environment or a distribution (`%optflags`) are kept. |
| `DRAGONGL_USE_CLANG` | `ON` | Pick `clang` when neither `$CC` nor `CMAKE_C_COMPILER` is set. `OFF`: the toolchain decides. |
| `DRAGONGL_BUILD_SERVER` | `ON` | Build the server, the world generator and the launchers; needs libsodium. |
| `DRAGONGL_BUILD_CLIENT` | `ON` | Build the Vulkan client and its shaders; needs Vulkan, GLFW and glslc. |
| `DRAGONGL_HARDENING` | `ON` | PIE, `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2` (not in Debug, nor when the flags already set it) and full RELRO. Distribution builds turn it off: their own flags do the same. |
| `DRAGONGL_INSTALL_SYSTEMD_UNIT` | `ON` | Install `dragongl-server.service` (server builds only). |
| `DRAGONGL_SYSTEMD_UNITDIR` | `lib/systemd/system` | Unit directory, relative to the prefix or absolute. |

Warnings are always `-Wall -Wextra -Wpedantic -Wformat -Wformat-security`; `./build.sh -w` adds `-Werror`.

**Version:** the single source of the version is the `Version:` line of `dragongl.spec` (`YYYY.MM.DD.NN`). CMake reads it at configure time and generates `generated/dragongl_config.h` (`DRAGONGL_VERSION`, `DRAGONGL_PKGDATADIR`) and the man pages from `man/*.1.in`; editing the spec re-runs the configuration. Both binaries print it with `--version`. `tools/check_release.py` checks that the spec, the newest `CHANGELOG.md` entry and its date agree (run in CI and in the RPM `%check`).

### Step 2: Compile
Build all executables and compile shaders in parallel:

```bash
cmake --build build --parallel $(nproc)
```

During the build:
1. `glslc` compiles all vertex, fragment, and GPD compute shaders (`shaders/*.comp`, `*.vert`, `*.frag`) into binary SPIR-V files inside `build/shaders/`.
2. The server (`build/dragongl_server`) and client (`build/dragongl_client`) executables are linked.

### Step 3: Install (optional)

```bash
DESTDIR=/tmp/stage cmake --install build   # or: sudo cmake --install build
```

The layout is `bin/`, `share/dragongl/{data,shaders,tools}`, `share/man/man1`, the unit in `DRAGONGL_SYSTEMD_UNITDIR` and `/etc/dragongl/dragongl-server.conf` (mode 0600; under `<prefix>/etc` for a prefix other than `/usr`). The installed binaries look for their data and shaders under `<prefix>/share/dragongl`.

---

## 4. Running the Test Suite

Dragon GL contains an independent, self-contained unit test suite in the `tests/` directory:

```bash
# Configure standalone test directory
cmake -B build-tests tests -DCMAKE_BUILD_TYPE=Debug

# Build test executables
cmake --build build-tests --parallel $(nproc)

# Run test runner
ctest --test-dir build-tests --output-on-failure
```

### Included Tests:
- `test_rules`: Verifies d20 polyhedral dice math, ability checks, and advantage/disadvantage mechanics.
- `test_protocol`: hand-written checks of the wire codec: header layout, round trips, malformed payloads.
- `test_protocol_conformance`: generated by `tools/gen_protocol.py` from `protocol/messages.json`: every example of the table must encode to the bytes the Python codec computes and decode back, every invalid variant must be refused.
- `test_tls`: fingerprint spelling, the ALPN protocol, and the `known_servers` store of the client.
- `test_pathfinding`: Validates A* navigation and collision routines across dungeon grids.
- `test_tome`: Verifies grimoire parsing and spell level progression requirements.
- `test_vfx_threads`: Stress-tests multithreaded VFX and Floating Combat Text (FCT) synchronization under heavy load.
- `gpu_mesh_test`: Verifies Vulkan GPD compute shader geometry expansion.

### Server Integration Tests

`tests/integration/` is a second standalone project. It builds the server with the flags you pass (normally ASan/UBSan) and runs three kinds of tests:

- `protocol_vectors` (`test_protocol_vectors.py`): the Python codec of the protocol table against the generated vectors `tests/protocol_vectors.json`.
- `server_network` (`test_server.py`): starts a real `dragongl_server` on a free port in a private copy of a generated world and drives it with a TLS protocol-v5 client (`dglproto.py`, on the Python codec of the protocol table). The first run generates the world and takes about a minute.
- `server_harness`: links the server sources without `main()` (`-DDRAGONGL_NO_MAIN`) and calls the code directly: persistence, trade, the command table, doors, movement, the per-floor NPC walk and the entity lookup.

```bash
cmake -B build-integration -G Ninja tests/integration -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-integration --parallel $(nproc)
ctest --test-dir build-integration --output-on-failure
```

Only a C compiler, CMake and Python 3 are required: no graphics stack. Tests that reproduce a defect still open are marked `WILL_FAIL` (CTest) or `@unittest.expectedFailure` (Python), tagged with the issue id. A fix turns them into an unexpected success: remove the marker in the same commit.

---

## 5. Local RPM Packaging Check

The spec builds every package and runs `tools/check_release.py` plus the unit suite in `%check`. CI does the same in a Fedora container (job `rpm`). By hand, on Fedora:

```bash
sudo dnf install -y rpm-build rpmlint dnf-plugins-core && sudo dnf builddep -y dragongl.spec
mkdir -p ~/rpmbuild/SOURCES
git archive --prefix=dragongl-$(rpmspec -q --qf '%{version}' --srpm dragongl.spec)/ \
    -o ~/rpmbuild/SOURCES/dragongl-$(rpmspec -q --qf '%{version}' --srpm dragongl.spec).tar.gz HEAD
rpmbuild -ba dragongl.spec
rpmlint -r dragongl.rpmlintrc ~/rpmbuild/RPMS/*/dragongl*.rpm ~/rpmbuild/SRPMS/dragongl*.rpm
```

The spec uses `%autorelease` and `%autochangelog` (as COPR builds do from git); outside an rpmautospec setup, replace them with a plain `Release:` and `%changelog` as the CI job does. `.gitattributes` keeps `readme_assets/` and `.github/` out of `git archive`.