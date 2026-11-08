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
- **Cryptography (server):** `libsodium` (Argon2id password hashing, constant-time comparisons)
- **Python:** Python 3 (for build verification and developer balance tools)

### Installing Dependencies on Fedora:
```bash
sudo dnf install -y \
    cmake \
    clang \
    compiler-rt \
    glslc \
    pkgconf-pkg-config \
    glfw-devel \
    vulkan-loader-devel \
    libsodium-devel \
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
| `-S` | build only the `dragongl_server` target |
| `-a` | also run cppcheck, with the same options as CI |
| `-x` | also install into `build/stage` (prefix `/usr`) to check the install layout |
| `-k` | also run the client smoke test (`tests/client/test_client.py`): a real server and the real client under Xvfb with Mesa lavapipe and the Vulkan validation layer — login, rendering, window resizes, Escape, server shutdown; any validation message fails. Needs Xvfb, `mesa-vulkan-drivers` (Fedora: `mesa-vulkan-drivers`), the validation layer (`vulkan-validation-layers` / `vulkan-validationlayers`) and `python3-xlib` |
| `-w` | warnings are errors (as in CI) |
| `-g` | gcc instead of clang |
| `-j N` | parallel jobs (default: number of CPUs) |
| `-v` | echo the commands and show the full output |

Options combine: `./build.sh -cn`, `./build.sh -dwa`, `./build.sh -j8 -i`. Conflicting options (two build types, `-t` with `-c`) are rejected. Every option that changes the compiler flags is passed to CMake on each run, so switching between `-d` and a Release build in the same directory never keeps stale sanitizer flags. The script prints a summary with the duration of each step and exits with a non-zero status on the first failure.

Build directories: `build/` (used by `./dragongl-server` and `./dragongl-client`), `build-tests/` (`build-tests-asan/` with `-d`), `build-integration/`. Sanitizer builds with clang need compiler-rt (Fedora: `compiler-rt`, Debian/Ubuntu: `libclang-rt-dev`); `-g` uses gcc instead.

The sections below describe the same steps done by hand.

---

## 3. Manual Build Workflow

### Step 1: Configure
Generate build files using an out-of-source directory:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**Build Configuration Options:**
- `-DCMAKE_BUILD_TYPE=Release`: Full optimizations (`-O3 -D_FORTIFY_SOURCE=2`) and security hardening.
- `-DCMAKE_BUILD_TYPE=Debug`: Debug symbols (`-g -O0`) without stripping.
- `-DDRAGONGL_USE_CLANG=ON` *(Default)*: Automatically defaults to `clang` if no compiler is explicitly set in `$CC` or `-DCMAKE_C_COMPILER`. Set to `OFF` to follow generic system toolchain defaults.

### Step 2: Compile
Build all executables and compile shaders in parallel:

```bash
cmake --build build --parallel $(nproc)
```

During the build:
1. `glslc` compiles all vertex, fragment, and GPD compute shaders (`shaders/*.comp`, `*.vert`, `*.frag`) into binary SPIR-V files inside `build/shaders/`.
2. The server (`build/dragongl_server`) and client (`build/dragongl_client`) executables are linked against their respective backends.

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
- `test_protocol`: Validates network packet packing, header structures, and `PROTOCOL_VERSION 3` negotiation.
- `test_pathfinding`: Validates A* navigation and collision routines across dungeon grids.
- `test_tome`: Verifies grimoire parsing and spell level progression requirements.
- `test_vfx_threads`: Stress-tests multithreaded VFX and Floating Combat Text (FCT) synchronization under heavy load.
- `gpu_mesh_test`: Verifies Vulkan GPD compute shader geometry expansion.

### Server Integration Tests

`tests/integration/` is a second standalone project. It builds the server with the flags you pass (normally ASan/UBSan) and runs two kinds of tests:

- `server_network` (`test_server.py`): starts a real `dragongl_server` on a free port in a private copy of a generated world and drives it with a protocol-v3 client (`dglproto.py`). The first run generates the world and takes about a minute.
- `server_harness`: links the server sources without `main()` (`-DDRAGONGL_NO_MAIN`) and calls persistence and trade code directly.

```bash
cmake -B build-integration -G Ninja tests/integration -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-integration --parallel $(nproc)
ctest --test-dir build-integration --output-on-failure
```

Only a C compiler, CMake and Python 3 are required: no graphics stack. Tests that reproduce a defect still open are marked `WILL_FAIL` (CTest) or `@unittest.expectedFailure` (Python), tagged with the issue id. A fix turns them into an unexpected success: remove the marker in the same commit.

---

## 5. Local RPM Packaging Check

To verify Fedora package compliance locally without submitting to COPR:

```bash
./local_check.sh
```

This creates an isolated local build tree (`_local_build`), packages the source tarball, executes `rpmbuild`, and runs `rpmlint` against the generated RPMs.
