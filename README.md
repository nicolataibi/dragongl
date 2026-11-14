# Dragon GL: The Long Descent into the Dragon's Maw

[![Version](https://img.shields.io/badge/Version-2026.11.09.02-green.svg)](https://github.com/nicolataibi/dragongl)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL%203.0--or--later-blue.svg)](LICENSE.txt)
[![Assets: CC BY 4.0](https://img.shields.io/badge/Assets-CC%20BY%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by/4.0/)
[![Language: C11](https://img.shields.io/badge/Language-C11-blue.svg)](CMakeLists.txt)
[![Graphics](https://img.shields.io/badge/Graphics-Vulkan%201.3-orange.svg)](shaders/)
[![Platform](https://img.shields.io/badge/Platform-Fedora%20Linux-blue?logo=fedora)](https://copr.fedorainfracloud.org/coprs/nicolataibi/dragongl/)

> **Multi-User Client-Server 3D RPG Engine — Vulkan**

---

<table align="center">
  <tr>
    <td align="center">
      <div align="center">
        <img src="readme_assets/logo/AIlogo.png" alt="AI Logo" width="400">
        <img src="readme_assets/logo/dragongllogo.png" alt="Dragon GL logo" width="400">
      </div>
    </td>
  </tr>
</table>

---

**Dragon GL** is a high-performance, 3D multi-user client-server dungeon exploration simulator and RPG engine. Built from the ground up in modern C11, the engine features a fully functional open Fantasy RPG mechanical core (d20 ruleset) and a real-time 3D client built on **Vulkan 1.3 (GPU-driven, dynamic rendering)**.

- **Upstream Website:** [https://github.com/nicolataibi/dragongl](https://github.com/nicolataibi/dragongl)
- **Fedora COPR Repository:** [https://copr.fedorainfracloud.org/coprs/nicolataibi/dragongl/](https://copr.fedorainfracloud.org/coprs/nicolataibi/dragongl/)
- **Author:** Nicola Taibi (assisted by Gemini, ChatGPT, Qwen, Gemma)
- **Code License:** GPL-3.0-or-later
- **Asset License:** CC BY 4.0 (Creative Commons Attribution 4.0 International)

---

## Screenshots

<p align="center">
  <img src="readme_assets/screenshot/scr0.png" alt="Dragon GL Gameplay Overview" width="750" />
</p>

<p align="center">
  <img src="readme_assets/screenshot/scr1.png" alt="Dungeon Exploration" width="370" />
  <img src="readme_assets/screenshot/scr2.png" alt="Combat and HUD" width="370" />
</p>

<p align="center">
  <img src="readme_assets/screenshot/scr4.png" alt="Voxel World Rendering" width="370" />
  <img src="readme_assets/screenshot/scr3.png" alt="Sanctuary City" width="370" />
</p>

---

## Key Features

### 1. GPU-Driven Vulkan Renderer
- **Vulkan 1.3, GPU-driven (GPD):** dynamic rendering, no render passes. Compute shaders (`gpd_cull_map.comp`, `gpd_cull_dyn.comp`, `gpd_expand_*.comp`, `gpd_final.comp`) cull the instances around the player, generate their geometry into a storage buffer and write the indirect draw command; a single `vkCmdDrawIndirect` renders the scene.
- **Robust presentation:** three frames in flight, one render-finished semaphore per swapchain image, swapchain rebuilt on resize and on `OUT_OF_DATE`/`SUBOPTIMAL`; resizable window (80% of the screen by default); best GPU picked automatically (`DRAGONGL_GPU` to override). Clean under the Khronos validation layer.
- **Spell VFX:** a particle engine renders the spell effects (projectiles, radial explosions, auras) sent by the server in `MSG_SPELL_VFX`.

### 2. High-Performance Client-Server Engine
- **Non-blocking Multiplexing:** Server handles up to 64 concurrent players on one `epoll` loop that never waits on a socket.
- **Encrypted, authenticated connections:** TLS 1.3 (OpenSSL 3). The server creates its certificate at the first start; the client pins its fingerprint at the first connection (or takes it from `--fingerprint`) and refuses a changed one. One protocol table (`protocol/messages.json`) generates the codec, its conformance tests and the protocol specification.
- **D3ES (Data-Driven Decoupled Entity System):** Static archetypes/blueprints are segregated from lightweight dynamic instances for cache-efficient processing.
- **Constant-Time $O(1)$ Spatial Partitioning:** Occupancy grid (`entity_grid`) provides instant collision detection, line-of-sight checks, and target queries.
- **FloorStats Cache:** Incremental lifecycle tracking (`floor_stats_npc_died()`, `floor_stats_npc_spawned()`) eliminates expensive full-map iterations.

### 3. Open RPG Ruleset Implementation (d20)
- **Polyhedral Dice Math:** True RNG engine supporting d4, d6, d8, d10, d12, and d20 dice rolls with Advantage/Disadvantage mechanics and Saving Throws.
- **52 Playable Ancestries (Races):** Extensive variety including Human, Elf, Dwarf, Gnome, Orc, Dragon-kin, Giant-kin, Celestial-blood, Fiend-blood, and dozens of exotic lineages.
- **12 Heroic Professions (Classes):** Wizard, Paladin, Cleric, Druid, Fighter, Monk, Ranger, Rogue, Sorcerer, Warlock, Barbarian, and Bard.
- **Survival Mechanics:** Realistic hunger bar, exhaustion levels, 10+ status conditions (Poisoned, Blinded, Paralyzed, Burning, Bleeding, Petrified, Frozen, Cursed, etc.), and end-of-turn recovery rolls.
- **Geology & Mining System:** Interactive voxel mining (`examine`, `tunnel <dir>`) with tool validation (Picks, Shovels) allowing players to extract gold veins, silver, and precious crystals.

### 4. World Dynamics & Progression
- **100 Procedurally Generated Floors:** Created using cellular automata and Perlin noise.
- **Floor 0 Safe Sanctuary:** Open-air circular hub featuring 11 specialized merchant shops, 8 class temples, 4 martial training dojos, sacred fountains, and meditation crystal statues.
- **Boss Gating (Every 10 Floors):** Floors 10, 20, 30... 100 house massive boss encounters dynamically drawn from native depth pools. Defeating the boss illuminates a trophy on the client HUD and unlocks deeper stairs.
- **Wandering Merchants (Every 5 Floors):** Errant traders (Blacksmith, Alchemist, Scroll Peddler, Wand Weaver, Arcane Smuggler) appear in deep dungeon levels with depth-scaled inventories.
- **Multiplayer Economy & Social Features:** Charisma-based haggling mini-game (`haggle`), Party system with shared XP, synchronized two-phase item trading, and local Chebyshev-distance spatial chat.

---

## Package Architecture (Fedora COPR / RPM)

The Fedora packaging divides Dragon GL into modular subpackages conforming to the Fedora Packaging Guidelines:

| Package | Architecture | Contents |
| :--- | :--- | :--- |
| **`dragongl`** | Arch-dependent | Vulkan client (`dragongl-client`), its SPIR-V shaders (`/usr/share/dragongl/shaders/`) and man pages. |
| **`dragongl-server`** | Arch-dependent | Server (`dragongl-server`), world reset utility (`dragongl-reset-world`), world generator, systemd unit `dragongl-server.service` and `/etc/dragongl/dragongl-server.conf`. |
| **`dragongl-data`** | `noarch` | Game datasets (`bestiary.json`, `items.json`, `spells.json`) in `/usr/share/dragongl/data/`. |
| **`dragongl-tools`** | `noarch` | DM and developer tools: balance analyzer (`analyze_balance.py`), map PDF generator (`generate_pdf_map.py`), and the HTML Data Studio suite. |
| **`dragongl-doc`** | `noarch` | User manuals and technical documents (`/usr/share/doc/dragongl/`). |

The artwork in `readme_assets/` (CC BY 4.0, see `readme_assets/LICENSE.md`) stays in the repository: it is not part of the release tarball nor of any package. Service setup: [INSTALL.md](INSTALL.md).

---

## Installation on Fedora Linux (via COPR)

Pre-built RPM packages for Fedora are available through Fedora COPR:

```bash
# 1. Enable the COPR repository
sudo dnf copr enable nicolataibi/dragongl

# 2. Client, or server host (pulls in dragongl-data)
sudo dnf install dragongl
sudo dnf install dragongl-server dragongl-tools
```

---

## Building from Source

### 1. Build Prerequisites (Fedora Linux)

Install the required compiler, build tools, and graphics development headers:

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

### 2. Compilation via CMake

Dragon GL uses standard CMake out-of-source builds with clang:

```bash
# Release build of client and server, then both test suites
./build.sh

# The same by hand
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

`./build.sh -S` builds the server only (no Vulkan or GLFW needed); all switches and CMake options are in [BUILD.md](BUILD.md).

---

## Running Dragon GL

### 1. Launching the Server (`dragongl-server`)

The server manages world state, NPC AI, combat resolution, and multiplayer synchronization:

```bash
# Interactive: the launcher asks for the server and DM passwords
./dragongl-server

# Passwords from files (first line, chmod 600) and a custom port
./dragongl-server --password-file server.pw --dm-password-file dm.pw --port 8080

# Explicit data directory path
./dragongl-server --password-file server.pw --data-dir /path/to/data
```

Passwords are never accepted on the command line: they come from files, from `DRAGONGL_SERVER_PASSWORD` / `DRAGONGL_DM_PASSWORD`, or from the interactive prompt. Each one has 8-63 printable ASCII characters without spaces. Without a DM password the `dm` account is disabled.

The server logs the fingerprint of its TLS certificate at start-up (`TLS fingerprint: SHA256:...`); players compare it with what their client prints at the first connection. `--tls-cert` / `--tls-key` use a certificate of your own instead of the generated one.

*Data directory resolution order:* `--data-dir` argument -> `$DRAGONGL_DATA_DIR` -> `data/` of the source tree (binary in `build/`) -> `/usr/share/dragongl/data` (installed). The current directory is never searched. On an RPM install the server usually runs as the `dragongl-server` systemd service ([INSTALL.md](INSTALL.md)).

### 2. Launching the Client (`dragongl-client`)

The client renders with Vulkan 1.3 (a GPU and driver with Vulkan 1.3 are required):

```bash
# Asks for the server host, then for the credentials
./dragongl-client

# Connect to a remote server (host name, IPv4 or IPv6 address)
./dragongl-client 192.168.1.50 8080

# Trust only the certificate the server owner gave you
./dragongl-client --fingerprint SHA256:0E:0D:...:C9 game.example.org

# Window size, GPU choice, validation layer
DRAGONGL_WINDOW=1920x1080 DRAGONGL_GPU=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./dragongl-client

# A larger HUD; your own keys
./dragongl-client --ui-scale 2
./dragongl-client --print-config > ~/.config/dragongl/client.conf
```

The HUD follows the monitor scale (HiDPI) and the size of the window. Settings and keys live in `~/.config/dragongl/client.conf`: `ui_scale`, `reduce_motion`, `show_fps`, two keys for every action (`key.move_north = W, Up`) and keys that send a command (`bind.F5 = look`). What the client does for accessibility, and how it is tested, is in [ACCESSIBILITY.md](ACCESSIBILITY.md); the tests to run on a real GPU are in [docs/GPU_TESTING.md](docs/GPU_TESTING.md).

### 3. Resetting the Game World (`dragongl-reset-world`)

To wipe temporary server saves and reset world persistence:

```bash
./dragongl-reset-world
```

---

## Player Quick Reference & Controls

Dragon GL features a classic NetHack/Angband-style command interface alongside real-time 3D movement. Commands are typed in the window (or in the terminal that started the client); the answers appear in the message panel.

### Keys

| Key | Action |
| :--- | :--- |
| `W` `A` `S` `D`, arrows | Step north, west, south, east |
| `Enter`, `/` | Type a command: `Enter` sends it, `Escape` cancels, `Up`/`Down` recall the earlier ones |
| `Q` `E`, `R` `F` | Turn the camera; raise and lower it (or drag with the mouse) |
| `PageUp` `PageDown` | Zoom (or the mouse wheel) |
| `Shift+PageUp` `Shift+PageDown` | Older and newer messages |
| `Ctrl+=` `Ctrl+-` | Larger and smaller HUD |
| `Tab` | Equipment and boss trophies on/off |
| `F3` | Frame time on/off |
| `F1` | The list of keys in use |
| `Escape` | Leave the game |

Every key can be changed in `client.conf`.

### Commands

In the game, `help` lists the topics (exploring, character, items, magic, merchants, trade, party and chat), `help <topic>` the commands of one, and `help <command>` explains a command. The same reference, generated from the command table of the server, is [docs/COMMANDS.md](docs/COMMANDS.md). To start with:

| Command | What it does |
| :--- | :--- |
| `look` (`l`) | What is around you and under your feet |
| `inventory` (`i`), `wear <n>`, `use <n>` | The backpack; put an item on; drink, read, eat or zap one |
| `get`, `drop <n>` | Pick up from the ground; drop |
| `status`, `spells`, `cast <spell>` | The character sheet; the spells you know; cast one |
| `talk`, `list`, `buy <n>`, `sell <n>` | Next to a merchant of the town |
| `rest` | In town: recover hit points and spell slots |
| `say <text>`, `msg <player> <text>`, `who` | Chat |

Spells are learned only from books and cast only with their book at hand. That includes the class transit cantrips, which move you between the surface and the deepest floor you have reached: each class has its own travel tome (for example *Primer of Arcane Transit* for the Wizard), sold by the book shops of floor 0. A command typed without its arguments answers with its usage; anything else that is not a command is reported as unrecognized.

---

## Dungeon Master (DM) & Developer Tools

Dragon GL includes a dedicated administrative role for game masters and automated balance analytics:

### DM Privileges & Commands

To authenticate as DM: connect with player name `dm` and the DM password configured at server startup (`--dm-password-file` or `DRAGONGL_DM_PASSWORD`; without it the DM account is disabled):

The DM has commands of its own (`dm_goto`, `dm_spawn`, `dm_boss`, `dm_merchant`, `dm_place`, `dm_gold`, `dm_level`, `dm_pdf`, `dm_telemetry`, ...): `help dm` in the game, or the last section of [docs/COMMANDS.md](docs/COMMANDS.md).

---

## Documentation Index

- [HOWTO.md](HOWTO.md) — Quick-start guide and basic invocation commands
- [LORE.md](LORE.md) — Comprehensive narrative, dungeon chronicle, and chapter lore
- [docs/COMMANDS.md](docs/COMMANDS.md) — Every command of the game (generated from the server)
- [docs/RELEASING.md](docs/RELEASING.md) — How a release is made and published
- [PROTOCOL.md](PROTOCOL.md) — Network wire protocol and packet specification
- [SECURITY.md](SECURITY.md) — Security policy and vulnerability reporting
- [LICENSE.txt](LICENSE.txt) — GNU General Public License v3.0 text
- [dragongl.spec](dragongl.spec) — Fedora RPM and COPR packaging specification

---

## Authors and Acknowledgments

- **Lead Developer & Creator:** Nicola Taibi
- **AI Design & Development Assistance:** Gemini (Google AI), ChatGPT (OpenAI), and local models Qwen & Gemma.
- **Graphical Assets & Artworks:** Curated and prompted by Nicola Taibi, generated via AI tools, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- **License:** Software licensed under [GPL-3.0-or-later](LICENSE.txt).
