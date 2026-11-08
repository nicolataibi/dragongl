# Dragon GL — Installation Guide

This document describes how to install Dragon GL on Linux systems, either via pre-built RPM packages from the Fedora COPR repository or by compiling and installing from source.

---

## 1. System Requirements

- **Operating System:** Linux (Fedora 40+, CentOS Stream, RHEL 9+, Ubuntu 22.04+, Debian 12+)
- **Architecture:** x86_64
- **Graphics Support:**
  - **OpenGL:** OpenGL 3.3+ with GLFW support
  - **Vulkan:** Vulkan 1.3/1.4 dynamic rendering compatible GPU and drivers
- **Runtime Dependencies:**
  - Standard C runtime (glibc)
  - GLFW 3.3+ (`glfw`)
  - Mesa OpenGL (`mesa-libGL`, `mesa-libGLU`)
  - Vulkan loader (`vulkan-loader`)
  - Python 3 (`python3`, required for `dragongl-tools`)

---

## 2. Installation via Fedora COPR (Recommended for Fedora / RHEL)

Pre-built RPM packages are available through Fedora COPR.

```bash
# 1. Enable the COPR repository
sudo dnf copr enable nicolataibi/dragongl

# 2. Install all Dragon GL packages (client, server, data, tools, documentation)
sudo dnf install dragongl dragongl-data dragongl-tools dragongl-doc
```

### Installed Package Components:
- **`dragongl`**: Client and server executables (`dragongl-server`, `dragongl-client`, `dragongl-reset-world`), symlinks, and man pages.
- **`dragongl-data`**: Core game databases (`bestiary.json`, `items.json`, `spells.json`) and compiled SPIR-V shaders located in `/usr/share/dragongl/`.
- **`dragongl-tools`**: Analysis utilities (`analyze_balance.py`, `generate_pdf_map.py`) and the Web Data Studio located in `/usr/share/dragongl/tools/`.
- **`dragongl-doc`**: Offline manual, guides, and documentation assets in `/usr/share/doc/dragongl/`.

---

## 3. Installation from Source via CMake

If you prefer building and installing from source:

### Prerequisites (Fedora Linux):
```bash
sudo dnf install -y \
    cmake \
    clang \
    compiler-rt \
    mesa-libGL-devel \
    mesa-libGLU-devel \
    glfw-devel \
    vulkan-loader-devel \
    glslc \
    pkgconf-pkg-config \
    python3
```

### Build and Install:
```bash
# Clone or enter the repository
cd dragongl-dev

# Configure the build with Release profile
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local

# Compile in parallel
cmake --build build --parallel $(nproc)

# Install binaries, data, shaders, and man pages
sudo cmake --install build
```

---

## 4. Runtime File Locations & Data Resolution

Dragon GL resolves its assets automatically at runtime:
1. **Explicit flag:** If the `--data-dir /path/to/data` argument is passed to `dragongl-server`, that directory is prioritized.
2. **Local development:** `./data` relative to the current working directory.
3. **System install:** `/usr/share/dragongl/data` (when installed from RPM or system prefix).

Mutable player saves (`.save`), combat logs (`combat_log.json`), and dynamic dungeon persistence are saved to the current working directory or user data directory, leaving system package files untouched.

---

## 5. Verification

After installation, verify that the binaries are properly installed and accessible:

```bash
# Verify client launcher
dragongl-client --help

# Verify server launcher
dragongl-server --help

# Verify tools
python3 /usr/share/dragongl/tools/analyze_balance.py --help
```

For quick-start usage, see [HOWTO.md](HOWTO.md) and [README.md](README.md).
