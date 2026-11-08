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
  - `glfw3` (GLFW development libraries)
  - `OpenGL` & `GLU` (Mesa development packages)
  - `Vulkan` loader and header files
- **System Libraries:** POSIX Threads (`pthread`), Math library (`libm`), PkgConfig (`pkg-config` / `pkgconf`)
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
    mesa-libGL-devel \
    mesa-libGLU-devel \
    vulkan-loader-devel \
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
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libvulkan-dev \
    python3
```

---

## 2. Standard Build Workflow

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

## 3. Running the Test Suite

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

---

## 4. Local RPM Packaging Check

To verify Fedora package compliance locally without submitting to COPR:

```bash
./local_check.sh
```

This creates an isolated local build tree (`_local_build`), packages the source tarball, executes `rpmbuild`, and runs `rpmlint` against the generated RPMs.
