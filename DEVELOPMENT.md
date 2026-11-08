# Dragon GL — Developer Guide

This document describes the internal architecture, threading model, subsystems, and developer workflows for Dragon GL.

---

## 1. Project Architecture

Dragon GL is designed with a clear separation between the server engine, the dual-backend graphics client, and shared game mechanics.

```
dragongl-dev/
├── include/           # Public headers (protocol, data structures, rules, voxels)
├── src/
│   ├── client/        # OpenGL & Vulkan frontends, input handling, HUD, sound/VFX
│   ├── server/        # TCP networking, D3ES entity engine, AI, dungeon generator
│   └── shared/        # d20 rules, cJSON parser, dataset loaders, map primitives
├── shaders/           # GLSL vertex, fragment, and GPD compute shaders
├── data/              # Read-only JSON datasets (bestiary.json, items.json, spells.json)
├── tests/             # Standalone C & Python unit test suite
├── tools/             # Balance analysis (Python) and Web Data Studio (HTML/JS)
└── readme_assets/     # Screenshots, portraits, and graphical assets
```

### Core Executables & Targets:
- **`dragongl_server`**: Authoritative game server simulating 100 floors, NPC AI, physics, traps, and combat.
- **`dragongl_client`**: 3D client rendering via OpenGL (GLFW) or Vulkan 1.4 GPU-Driven Dynamic Rendering.
- **`dragongl_shaders`**: Custom target invoking `glslc` to produce SPIR-V binaries (`shaders/*.spv`).

---

## 2. Rendering Pipelines (OpenGL vs. Vulkan)

Dragon GL implements two independent, visually synchronized rendering backends:

### A. Vulkan GPU-Driven Dynamic Rendering (GPD)
The modern renderer (`src/client/render_vk.c`) utilizes Vulkan 1.4 Dynamic Rendering and a Compute-Driven pipeline:
1. **Lightweight SSBO Push:** The CPU uploads compact instance descriptors (`GpdInstance` and `GpdOriented`) containing position, orientation, and color to mapped Shader Storage Buffer Objects.
2. **GPU Culling & Expansion:** Dedicated compute shaders (`gpd_cull_map.comp`, `gpd_cull_dyn.comp`, `gpd_expand_*.comp`) run directly on the GPU to perform frustum/distance culling and generate triangle primitives.
3. **Indirect Execution:** The compute pass writes geometry directly to vertex buffers and atomically populates the draw command (`gpd_final.comp`), dispatched via `vkCmdDrawIndirect`.
4. **Environment Fallback:** Setting `DRAGONGL_GPD=0` forces Vulkan to use the CPU-side vertex generator for debugging or older GPU hardware.

### B. OpenGL GLFW Backend
The OpenGL backend (`src/client/render_gl.c`) offers maximum compatibility across diverse Linux graphics drivers, generating batched vertex arrays on the CPU and rendering with standard GLSL shaders.

### Visual FX & HUD Parity
- **Floating Combat Text (FCT):** 3D world coordinates of damage/healing values are projected onto the 2D HUD viewport using the camera's Model-View-Projection (MVP) matrix.
- **Particle System:** Both backends consume `MSG_SPELL_VFX` packets to spawn framerate-independent, delta-time animated particles with school-dependent color coding.

---

## 3. Server Architecture: D3ES & Spatial Partitioning

### Data-Driven Decoupled Entity System (D3ES)
The server abandons monolithic structures in favor of lightweight composition:
- **`EntityTemplates`**: Immutable blueprints loaded from JSON datasets (`data/bestiary.json`, `data/items.json`).
- **`EntityInstance`**: Ephemeral, runtime state containing current HP, position `(x, y)`, level, and active status conditions.
- **Type-Safe Archetypes**: Categorized by `EntityArchetype` enums (`ARCH_SWARM`, `ARCH_BOSS`, `ARCH_MERCHANT`, `ARCH_CASTER`), replacing slow string comparisons with fast jump tables.

### Spatial Partitioning ($O(1)$)
Every floor maintains an `entity_grid` occupancy map. Looking up an entity at coordinates `(x, y)` or running melee collision detection runs in constant time $O(1)$, avoiding full entity-array scans.

### FloorStats Cache
NPC lifecycle events (`floor_stats_npc_died()`, `floor_stats_npc_spawned()`) update per-floor counters incrementally. Emergency respawn checks read `g_floor_stats[floor_id]` in $O(1)$ time without traversing dormant floors.

---

## 4. Threading & Synchronization Model

In the client, rendering and networking execute on separate POSIX threads:
- **Network Thread:** Manages TCP socket communication, parses incoming binary packets (`MSG_STATE`, `MSG_MAP_CHUNK`, `MSG_SPELL_VFX`), and updates the master state under `g_state_mutex`.
- **Render Thread:** Executes GLFW event polling and the Vulkan/OpenGL draw loop. To prevent data races, each frame captures a `FrameSnapshot` copy of volatile coordinates and maps.
- **VFX & FCT Mutexes:** Dynamic particle arrays and floating combat text use dedicated fine-grained locks (`g_particles_mutex`, `g_fct_mutex`) to allow high-frequency spawns without locking the network loop.

---

## 5. Developer & Balancing Workflows

### 1. Game Balance Analysis
Combat interactions are recorded as structured JSON records in `combat_log.json`, in the server state directory (`state/` in a development tree). Run the statistical analysis script to review balance data:

```bash
# Run balance analysis report (finds state/combat_log.json by itself)
python3 tools/analyze_balance.py
```

The script outputs four analytical domains:
- **Attack Performance:** Accuracy %, critical hit frequency, average damage per hit.
- **Saving Throw Health:** Pass/fail rates by effect type and DC.
- **Spell Metrics:** Output efficiency (damage/healing per cast) and resistance rates.
- **Lethality Map:** Floor-by-floor mortality ASCII chart and top killer units.

### 2. Web Data Studio
The `tools/web/` directory contains self-contained HTML/JS workbenches for dataset curation:
- `tools/web/menu.html`: Main navigation hub.
- `tools/web/bestiary.html`: Monster stats, archetypes, and abilities editor.
- `tools/web/items.html`: Weapon, armor, and magic artifact editor.
- `tools/web/spells.html`: Spell mechanics, progression tiers, and grimoires editor.

### 3. Dungeon Master (DM) Runtime Telemetry
Administrators logged in with DM credentials can monitor engine performance directly from the in-game console:
- `dm_telemetry`: Inspect active client count, live NPC population, and tick latency.
- `dm_boss <floor>`: Query the boss status and loot on a specific floor.
- `dm_mapfloor` / `dm_pdf <floor>`: Dump color-coded 2D layout PDFs of dungeon levels.

---

## 6. Testing & CI Standards

The quickest loop is `./build.sh` (Release build + both suites) or `./build.sh -dwa` (ASan/UBSan, `-Werror` and cppcheck, close to what CI runs); see `BUILD.md`.

The standalone test suite resides in `tests/` and mirrors production compiler flags. By hand:

```bash
# Compile and run unit tests
cmake -B build-tests tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --parallel $(nproc)
ctest --test-dir build-tests --output-on-failure

# Server integration tests (real server process + in-process harness)
cmake -B build-integration tests/integration -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-integration --parallel $(nproc)
ctest --test-dir build-integration --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs four jobs on every push and pull request: a clang Release build with `-Werror`, the unit tests (Release and ASan/UBSan), the server integration tests under ASan/UBSan, and cppcheck. cppcheck false positives are suppressed inline with `// cppcheck-suppress <id>` and a comment explaining why.

### Guidelines for Contributions:
1. **Warning-Clean Build:** All C code must compile cleanly with `-Wall -Wextra -Wpedantic` using clang.
2. **Data Structure Bounds:** Enum-indexed arrays must be sized using `MAX_*` sentinels to guarantee type safety without bitmask overflow.
3. **Persisted Formats:** Changes to `MsgHeader` or wire structures require incrementing `PROTOCOL_VERSION` in `include/protocol.h`.
