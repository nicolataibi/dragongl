# Dragon GL — Installation Guide

This document describes how to install Dragon GL on Linux systems, either via pre-built RPM packages from the Fedora COPR repository or by compiling and installing from source.

---

## 1. System Requirements

- **Operating System:** Linux (Fedora 40+, CentOS Stream, RHEL 9+, Ubuntu 22.04+, Debian 12+)
- **Architecture:** x86_64
- **Graphics Support (client):** a GPU and driver with Vulkan 1.3 and a queue that does graphics, compute and presentation (any current AMD, Intel or NVIDIA driver; Mesa `lavapipe` works too, slowly, without a GPU). The server needs no graphics stack.
- **Runtime Dependencies:**
  - Standard C runtime (glibc)
  - GLFW 3.3+ (`glfw`)
  - Vulkan loader (`vulkan-loader`) and a Vulkan driver (`mesa-vulkan-drivers`, or the vendor's)
  - libsodium (`libsodium`, server)
  - Python 3 (`python3`, required for `dragongl-tools`)

---

## 2. Installation via Fedora COPR (Recommended for Fedora / RHEL)

Pre-built RPM packages are available through Fedora COPR.

```bash
# 1. Enable the COPR repository
sudo dnf copr enable nicolataibi/dragongl

# 2a. Player machine: the client
sudo dnf install dragongl

# 2b. Server host: the server, its data and tools
sudo dnf install dragongl-server dragongl-tools
```

### Installed Package Components:
- **`dragongl`**: the Vulkan client (`dragongl_client`, launcher `dragongl-client`), its SPIR-V shaders in `/usr/share/dragongl/shaders/` and its man pages.
- **`dragongl-server`**: the server (`dragongl_server`, launchers `dragongl-server` and `dragongl-reset-world`), the world generator, the systemd unit `dragongl-server.service` and its configuration `/etc/dragongl/dragongl-server.conf`.
- **`dragongl-data`**: the game datasets (`bestiary.json`, `items.json`, `spells.json`) in `/usr/share/dragongl/data/`, required by the server.
- **`dragongl-tools`**: analysis utilities (`analyze_balance.py`, `generate_pdf_map.py`, used by the DM `dm_pdf` command) and the Web Data Studio in `/usr/share/dragongl/tools/`.
- **`dragongl-doc`**: the manuals in `/usr/share/doc/dragongl/`.

A player machine needs only `dragongl`; a server host needs only `dragongl-server` (it pulls in `dragongl-data` and recommends `dragongl-tools`).

### Running the Server as a Service

`dragongl-server` installs a systemd unit. It runs the server under a transient unprivileged user (`DynamicUser=`) with the state directory `/var/lib/dragongl` (mode 0700) and a strict sandbox:

```bash
# 1. Set the passwords (the file is root-only, mode 0600)
sudoedit /etc/dragongl/dragongl-server.conf

# 2. Start the service now and at boot
sudo systemctl enable --now dragongl-server

# 3. Follow the log
journalctl -u dragongl-server -f
```

The configuration file holds `DRAGONGL_SERVER_PASSWORD`, `DRAGONGL_DM_PASSWORD` (empty: the `dm` account is disabled) and `DRAGONGL_SERVER_ARGS` (extra options, for example `--port 9000`). Without a server password the service exits with status 2 and is not restarted. To start a new world, stop the service first: `sudo systemctl stop dragongl-server && sudo dragongl-reset-world -S /var/lib/dragongl`.

---

## 3. Installation from Source via CMake

If you prefer building and installing from source:

### Prerequisites (Fedora Linux):
```bash
sudo dnf install -y \
    cmake \
    clang \
    glfw-devel \
    vulkan-loader-devel \
    glslc \
    pkgconf-pkg-config \
    libsodium-devel \
    openssl-devel \
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

# Install binaries, data, shaders, man pages and the systemd unit
sudo cmake --install build
```

A server-only host needs neither Vulkan nor GLFW: configure with `-DDRAGONGL_BUILD_CLIENT=OFF` (and drop `glfw-devel`, `vulkan-loader-devel` and `glslc`). The build options are listed in [BUILD.md](BUILD.md). With a prefix other than `/usr` the unit goes to `<prefix>/lib/systemd/system`; pass `-DDRAGONGL_SYSTEMD_UNITDIR=/etc/systemd/system` to put it where systemd reads it, or `-DDRAGONGL_INSTALL_SYSTEMD_UNIT=OFF` to skip it.

---

## 4. Runtime File Locations & Data Resolution

The server finds its read-only game data in this order (the current directory is never searched):
1. `--data-dir <dir>` (or `-D`);
2. `$DRAGONGL_DATA_DIR`;
3. `data/` of the source tree, when the binary runs from a build directory inside it (`build/dragongl_server`);
4. `<datadir>/dragongl/data` of the install prefix (`/usr/share/dragongl/data` for the RPM).

Without data the server exits with status 2. The client finds its shaders the same way: `$DRAGONGL_SHADER_DIR`, then `shaders/` next to the binary (build tree), then `<datadir>/dragongl/shaders`.

Everything the server writes lives in its **state directory**, never in the data directory or next to the package files: `world.dat`, `npcs.dat`, `artifacts.dat`, `saves/` (characters and tombstones), `maps/` (DM maps), `combat_log.json`, and `tls/` (the server's TLS key and certificate, created at the first start). It is chosen in this order:

1. `--state-dir <dir>` (or `-S`);
2. `$DRAGONGL_STATE_DIR`;
3. `$XDG_STATE_HOME/dragongl`, else `~/.local/state/dragongl`.

The `dragongl-server` launcher of a development tree uses `state/` in the project root; the systemd service uses `/var/lib/dragongl`. The directory is created with mode 0700 and locked by the running server. `dragongl-reset-world` deletes its content to start a new world.

### TLS identity

Clients connect over TLS 1.3. At the first start the server creates `tls/server.key` (ECDSA P-256, mode 0600) and `tls/server.crt` (self-signed for the host name, 10 years) in its state directory and logs the certificate fingerprint:

```text
[SYS] TLS fingerprint: SHA256:0E:0D:AD:F6:...:82:36:C9
```

Publish that line to your players (`openssl x509 -in /var/lib/dragongl/tls/server.crt -noout -fingerprint -sha256` prints it too): their client records it at the first connection and refuses a server whose certificate changes, so keep `tls/` when you move or back up the world. `dragongl-reset-world` keeps it as well; delete it only to change the server identity on purpose.

A certificate of your own (for example one signed by a public authority for the server's host name, which clients then accept without pinning) goes in with `--tls-cert <file> --tls-key <file>`. For the systemd service, whose dynamic user cannot read other users' files, pass them as credentials with a drop-in (`systemctl edit dragongl-server`):

```ini
[Service]
LoadCredential=tls.crt:/etc/dragongl/tls.crt
LoadCredential=tls.key:/etc/dragongl/tls.key
```

and in `/etc/dragongl/dragongl-server.conf`:

```sh
DRAGONGL_SERVER_ARGS=--tls-cert /run/credentials/dragongl-server.service/tls.crt --tls-key /run/credentials/dragongl-server.service/tls.key
```

---

## 5. Verification

After installation, verify that the binaries are properly installed and accessible:

```bash
# Versions (the same on both binaries)
dragongl_client --version
dragongl_server --version

# Verify client launcher
dragongl-client --help

# Verify server launcher
dragongl-server --help

# Verify tools
python3 /usr/share/dragongl/tools/analyze_balance.py --help
```

For quick-start usage, see [HOWTO.md](HOWTO.md) and [README.md](README.md).
