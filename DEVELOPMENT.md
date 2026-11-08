# Dragon GL — Developer Guide

This document describes the internal architecture, threading model, subsystems, and developer workflows for Dragon GL.

---

## 1. Project Architecture

Dragon GL is designed with a clear separation between the server engine, the Vulkan client, and shared game mechanics.

```
dragongl-dev/
├── include/           # Public headers (protocol, data structures, rules, voxels)
├── src/
│   ├── client/        # Vulkan renderer, input handling, HUD, VFX, network thread
│   ├── server/        # Event loop, game tick, movement, command table + handlers, AI, persistence
│   └── shared/        # d20 rules, cJSON parser, dataset loaders, map primitives
├── shaders/           # GLSL vertex, fragment, and GPD compute shaders
├── data/              # Read-only JSON datasets (bestiary.json, items.json, spells.json)
├── tests/             # Standalone C & Python unit test suite
├── tools/             # Balance analysis (Python) and Web Data Studio (HTML/JS)
└── readme_assets/     # Screenshots, portraits, and graphical assets
```

### Core Executables & Targets:
- **`dragongl_server`**: Authoritative game server simulating 100 floors, NPC AI, physics, traps, and combat.
- **`dragongl_client`**: 3D client, Vulkan 1.3 GPU-driven dynamic rendering (GLFW window).
- **`dragongl_shaders`**: Custom target invoking `glslc` to produce SPIR-V binaries (`shaders/*.spv`).

---

## 2. Rendering: Vulkan 1.3, GPU-Driven

The client has a single renderer (`src/client/render_vk.c`, `src/client/vk_init.c`): Vulkan 1.3 with dynamic rendering and synchronization2, the only device features it enables.

1. **Instance upload:** the CPU writes compact instance descriptors (`GpdInstance`, `GpdOriented`: position, scale, colour, orientation) into persistently mapped storage buffers, one set per frame slot. Map tiles are rebuilt only for a window around the player and only when the map changed.
2. **GPU culling and expansion:** compute shaders (`gpd_cull_map.comp`, `gpd_cull_dyn.comp`, `gpd_expand_*.comp`) drop what lies beyond the vision radius and generate the triangles into a device-local vertex buffer.
3. **Indirect draw:** `gpd_final.comp` writes the `VkDrawIndirectCommand`; the scene is one `vkCmdDrawIndirect`, the HUD a second draw from a host vertex buffer.

**Frame loop** (`draw_frame`): wait for the fence of the slot (three frames in flight), acquire an image, reset the fence, record, submit, present. The image-available semaphore and the fence belong to the frame slot; the render-finished semaphore belongs to the swapchain image. A resize (GLFW callback), `VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR` rebuilds the swapchain (with `oldSwapchain`) before the next frame; a minimized window is waited out. Viewport and scissor are dynamic state, so pipelines survive a resize.

**Device selection:** every GPU is listed at start-up with the reason it is unusable, if any (no Vulkan 1.3, no swapchain, no graphics+compute queue that can present to the window). Discrete GPUs win over integrated, virtual and software ones, then the larger device-local memory. `DRAGONGL_GPU=<index|name>` overrides the choice.

**Shaders** are read from `$DRAGONGL_SHADER_DIR`, then `shaders/` next to the executable (`build/shaders`), then `/usr/share/dragongl/shaders`.

**Validation:** `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` enables the Khronos layer. `tests/client/test_client.py` runs the real client against a real server under Xvfb with Mesa lavapipe and the validation layer: it fails on any validation message, checks that frames are drawn, resizes the window and quits with Escape (`./build.sh -k`, and in CI).

### Visual FX & HUD
- **Floating Combat Text (FCT):** 3D world coordinates of damage/healing values are projected onto the 2D HUD viewport using the camera's Model-View-Projection (MVP) matrix.
- **Particle System:** `MSG_SPELL_VFX` packets spawn framerate-independent, delta-time animated particles with school-dependent color coding.

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

### NPC lifecycle, floor walks and entity lookups
- **`npc_set_active()`** (`server_world.c`) is the only place where an NPC becomes alive or dead: it keeps the floor counters and the per-floor index in step. Set the final fields (floor, template, archetype) first, then activate; never write `n->active` by hand.
- **Floor walk:** `FloorIter it = floor_iter(npcs, f); for (int i; (i = floor_iter_next(&it)) >= 0;)` visits the active NPCs of floor `f`, in slot order, exactly like a scan of all `MAX_NPCS` slots would (NPCs spawned during the walk included), in O(NPCs on the floor). Use it for every per-floor query instead of a full scan.
- **Entity id → NPC:** `npc_by_entity_id()` answers in O(1) from a map rebuilt every tick (an NPC spawned since then is found by one scan). Entity ids are unique: `next_id` only grows and is saved with the NPCs.

### Server source layout
- `main_server.c`: options, start-up, the event loop (`run_server`), message dispatch, logins, traps and tile events, shutdown.
- `server_move.c`: one step of a player (`MSG_MOVE`): bump into a merchant, loot, a monster or a player, or walk onto the tile.
- `server_world.c`: the game tick (`update_world`: autosave, global event, player rounds, NPC rounds, density monitor), the floor index and the entity map.
- Text commands: `server_commands.c` holds the command table and `handle_text_cmd()`; the handlers live in one file per group: `cmd_character.c` (help, train, stats, rest, save), `cmd_social.c` (party, trade, chat, block list, leaderboard), `cmd_shop.c` (merchants), `cmd_items.c` (inventory, use, get, drop, throw), `cmd_magic.c` (spells, cast), `cmd_world.c` (look, search, disarm, examine, tunnel, doors) and `cmd_dm.c` (the `dm_*` commands and their own table).

A new command is one handler `bool cmd_x(Client *c, const char *cmd, NPC *npcs)` (true when it handled the command) and one row of `COMMANDS[]`: exact names and/or prefixes separated by `|`. The table is walked in order and the first match whose handler returns true wins, so a prefix row covers every later row it matches (`"c <n>"` is cast, `"c"` alone is close). The `command_table` test of the harness checks that the commands answer.

---

## 4. Threading & Synchronization Model

The server runs the game on one thread (the `epoll` main loop, fixed 200 ms tick) and moves the slow work off it:
- **Writer thread** (`src/server/writer.c`): saves are encoded in memory by the game loop and written by this thread (write, `fsync`, `rename`). One pending job per file at most: a newer snapshot replaces a queued one. `writer_wait_for(path)` blocks until a file's pending writes are done. Before `writer_start()` and after `writer_stop()` every write is synchronous (startup, shutdown, tests).
- **Login worker** (`src/server/login_worker.c`): DM password check, save read and Argon2id verify/hash. `login_begin()` submits a job and marks the slot `login_pending`; the worker signals an `eventfd` and `login_finish()` applies the result on the main loop.
- **Child processes:** the DM PDF generator is spawned and reaped with `waitpid(WNOHANG)` once per loop pass (`pdf_jobs_poll()`).
- **Leaderboard:** `top` scans the saves at most once a minute and takes the characters online from memory.

In the client, rendering and networking execute on separate POSIX threads:
- **Network Thread:** Manages TCP socket communication, parses incoming binary packets (`MSG_STATE`, `MSG_MAP_CHUNK`, `MSG_SPELL_VFX`), and updates the master state under `g_state_mutex`.
- **Render Thread:** Executes GLFW event polling and the Vulkan draw loop. To prevent data races, each frame captures a `FrameSnapshot` copy of volatile coordinates and maps.
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
- `dm_mapfloor` / `dm_pdf <floor>`: Dump color-coded 2D layout PDFs of dungeon levels. The generator runs in the background (at most two at a time, one per floor); the DM is told when the PDF is ready.

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

CI (`.github/workflows/ci.yml`) runs seven jobs on every push and pull request: Release builds with clang and gcc and `-Werror`, the unit tests (Release and ASan/UBSan), the server integration tests under ASan/UBSan, the client smoke test under ASan/UBSan (Xvfb, lavapipe, Vulkan validation layer), a server-only build without any graphics package, the RPM build with rpmlint in a Fedora container, and static analysis (cppcheck and `tools/check_release.py`). cppcheck false positives are suppressed inline with `// cppcheck-suppress <id>` and a comment explaining why.

### Guidelines for Contributions:
1. **Warning-Clean Build:** All C code must compile cleanly with `-Wall -Wextra -Wpedantic` using clang.
2. **Data Structure Bounds:** Enum-indexed arrays must be sized using `MAX_*` sentinels to guarantee type safety without bitmask overflow.
3. **Wire and file formats:** a message is changed in one place, its encoder/decoder pair in `src/shared/wire.c` (plus `tests/test_protocol.c`, `tests/integration/dglproto.py` and `PROTOCOL.md`), and every change increments `PROTOCOL_VERSION` in `include/protocol.h`. On-disk formats have their own version numbers in `src/server/persist.h`.
4. **Function size:** no function longer than 300 lines; split by responsibility (see `update_world`, `check_traps`, the item and spell loaders).
5. **Comments say why the code is the way it is now.** The history of a change (what was wrong, which defect it fixed) belongs to `CHANGELOG.md`, not to the source.
