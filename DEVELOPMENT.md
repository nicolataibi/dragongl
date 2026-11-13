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
NPC lifecycle events (`floor_stats_npc_died()`, `floor_stats_npc_spawned()`) update per-floor counters incrementally. Emergency respawn checks read the counters of a floor (`floor_stats_get()`) in $O(1)$ time without traversing dormant floors.

### NPC lifecycle, floor walks and entity lookups
- **`npc_set_active()`** (`npc_index.c`) is the only place where an NPC becomes alive or dead: it keeps the floor counters and the per-floor index in step. Set the final fields (floor, template, archetype) first, then activate; never write `n->active` by hand.
- **Floor walk:** `FloorIter it = floor_iter(game, f); for (int i; (i = floor_iter_next(&it)) >= 0;)` visits the active NPCs of floor `f`, in slot order, exactly like a scan of all `MAX_NPCS` slots would (NPCs spawned during the walk included), in O(NPCs on the floor). Use it for every per-floor query instead of a full scan.
- **Entity id → NPC:** `npc_by_entity_id()` answers in O(1) from a map rebuilt every tick (an NPC spawned since then is found by one scan). Entity ids are unique: `game->next_id` only grows and is saved with the NPCs.
- **New NPCs:** `npc_alloc()` hands out a slot (one that never held a creature first, a dead monster last) and `npc_spawn_item()` puts an item on the ground; never look for a free slot by hand, the leftovers of its previous tenant (loot, name, timers) would come along.

### The game state: `Game`
The server has no global game state. `struct Game` (`src/server/game.h`) holds the world, the client and NPC tables, the round counter, the next entity id, the world event, the tombstones, the clouds and the data directory; `main()` creates one with `game_create()` and every function that reads or changes the game takes it as `Game *game` (the tests and the fuzzers own one too, `g_game` in `tests/integration/fixture.c`). What stays `static` inside a module is derived or technical: the per-floor NPC index and the entity map (`npc_index.c`, rebuilt from the NPC table), the connection table, the writer and login queues.

### Server source layout
The sources are layered, and `tools/check_deps.py` (`tools/quality.sh deps`, CI) refuses an include that points upward or a cycle among the headers:

| Layer | Files | Knows about |
| --- | --- | --- |
| shared | `include/`, `src/shared/` | rules, data, map, protocol; nothing of the server or the client |
| model | `server.h`, `server_entities.h`, `auth.h` | the types of the server |
| infra | `server_log`, `combat_log`, `persist`, `writer`, `login_worker`, `server_conn`, `server_tls`, `pathfinding` | files, sockets, threads; not the game being played |
| game | everything else | the simulation |
| commands | `cmd_*.c`, `server_commands.c` | the text commands of the players |
| main | `main_server.c`, `server_startup.c`, `server_login.c`, `server_options.c` | the composition of the program |

The client (`src/client/`) is a separate program and includes `shared` only.

- `main_server.c`: `main()`, the event loop (`run_server`), message dispatch, shutdown. `server_options.c` the command line, `server_startup.c` the data directory, the game data and the world (loaded or generated), `server_login.c` connections being accepted and logins being finished.
- `server_state.c`: `game_create()` / `game_destroy()`.
- `server_move.c`: one step of a player (`MSG_MOVE`): bump into a merchant, loot, a monster or a player, or walk onto the tile. `server_tiles.c` what the tile does (stairs, doors, entity grid), `server_traps.c` the traps.
- `server_world.c`: the game tick (`update_world`: autosave, global event, player rounds, NPC rounds, density monitor), `npc_alloc()`. `npc_index.c`: `npc_set_active()`, the floor counters, the per-floor index and the entity map.
- `server_combat.c` attacks and kills, `server_loot.c` what a monster drops and the artifacts, `server_spawn.c` the population of the city and of the dungeon, `tombstone.c` the remains of a dead character.
- `player_stats.c` what is computed from a character (totals, armor class, weight, level), `player_store.c` its save file, `inventory.c` finding and removing what it carries.
- Connections: `server_conn.c` owns every socket: TLS handshake, receive buffer, send queue, deferred removal and the refusal slots; `server_tls.c` the certificate and the TLS context; `server_net.c` encodes the messages for a client. The codec itself (`src/shared/wire.c`) is generated from `protocol/messages.json`, and `src/shared/tls_common.c` holds what client and server share (TLS 1.3 context, ALPN, fingerprints). On the client, `client_tls.c` runs the session and the trust decision, `known_servers.c` the trust-on-first-use store.
- Text commands: `server_commands.c` holds the command table and `handle_text_cmd()`; the handlers live in one file per group: `cmd_character.c` (help, train, stats, rest, save), `cmd_social.c` (party, trade, chat, block list, leaderboard), `cmd_shop.c` (merchants), `cmd_items.c` (inventory, lore, study, use), `cmd_equip.c` (wear, remove, belt), `cmd_ground.c` (pickup, get, drop, throw), `cmd_magic.c` (spells, cast), `cmd_world.c` (look, search, disarm, examine, doors), `cmd_mine.c` (tunnel) and `cmd_dm.c` (the `dm_*` commands and their own table).
- Shared code: `map.c` generates a dungeon floor, `map_city.c` the city, `map_traps.c` the traps; `data_loader.c` reads the bestiary and the spells, `data_items.c` the items.

A number typed by a player goes through `src/shared/strnum.c` (`parse_int`, `scan_ints`, `str_to_int`: no undefined behaviour, clamped) and is checked against its range before it indexes anything; a line of a 1-based list (`buy 3`) through `list_index()`.

A new command is one handler `bool cmd_x(Client *c, const char *cmd, Game *game)` (true when it handled the command) and one row of `COMMANDS[]`: exact names and/or prefixes separated by `|`, and a usage line. Every prefix ends with a space, so it matches the whole word followed by arguments (`"train "` never matches `trainer`); list the bare word among the names when the command also works without arguments. The table is walked in order and the first match whose handler returns true wins, so a prefix row covers every later row it matches (`"c <n>"` is cast, `"c"` alone is close). When no handler accepts the line, the player gets the usage of the row named by the first word, otherwise "Unrecognized command". The `command_table` and `command_words` tests of the harness check all this.

### Spells: data, resolution, balance
A spell does what the `effect` object of its record in `data/spells.json` says; nothing is read from the description. `data/schema/spells.schema.json` documents every field:

```json
"effect": {"kind": "damage", "target": "circle", "range": 30, "radius": 4,
           "roll": "save", "save": "dex", "on_save": "half",
           "dice": {"count": 8, "sides": 6}, "damage_type": "fire"}
```

- **kind**: `damage` (with an optional `condition` rider), `heal`, `condition`, `buff` (`stat` ac/attack/damage, `value`, `rounds`), `utility` (no combat mechanic), `special` (a `handler` of `spell_router.c`). **target**: `single`, `self`, `circle`, `cone`, `line`, `cloud`. Ranges and radii are cells of 5 feet. **roll**: `auto`, `attack` (spell attack against AC) or `save`; with an attack or no roll, `save` names the saving throw that resists only the rider. `dc` (fixed) is for martial maneuvers; spells use 8 + proficiency + the class casting ability. `cantrip_scaling` multiplies the dice at levels 5, 11 and 17. `terrain` (wall, ice, mud) is left at the origin after the effect.
- **Code**: `src/shared/spell_effect.c` parses the object (the server refuses to start on an invalid one), `src/shared/spell_rules.c` resolves an effect against one target with no world (attack, save, crit, half damage, rider, resistances), `src/server/spell_cast.c` picks the target and the area, applies the outcome and the rewards, `src/server/aoe.c` holds the area geometry, the terrain chemistry and the clouds.
- **A new handler**: a `SpellHandler` value (`include/spells.h`), its name in `k_handlers` (`spell_effect.c`), the function in `g_handlers[]` (`spell_router.c`), the name in the schema enum; `tools/check_data.py` checks that the schema and the engine list the same handlers and conditions.
- **Tests**: `tests/test_spell_rules.c` (parser and resolution, forced d20), the `spell_*` cases of the integration harness (one per kind of effect through the real `cast` command, and `spell_every_effect`: every spell of the data cast once, checked against its record), `tests/spell_balance.c` (below).
- **Balance**: `spell_balance` simulates every damage spell with the game's loader and resolution against the reference target of its level and checks the expected damage, heals, buffs and unresisted control against `tests/spell_balance.json`; `build-tests/spell_balance data tests/spell_balance.json --report` prints the whole table. A change of the data that breaks a limit fails CI: fix the spell, or move the limit in the same change with the reason in the changelog.

---

## 4. Threading & Synchronization Model

The server runs the game on one thread (the `epoll` main loop, fixed 200 ms tick) and moves the slow work off it:
- **Writer thread** (`src/server/writer.c`): saves are encoded in memory by the game loop and written by this thread (write, `fsync`, `rename`). A process killed at any point of that sequence leaves the previous file or the new one, never half of one (`crash_during_save` in the harness kills a child at each point; `test_killed_server_recovers` kills a real server). One pending job per file at most: a newer snapshot replaces a queued one. `writer_wait_for(path)` blocks until a file's pending writes are done. Before `writer_start()` and after `writer_stop()` every write is synchronous (startup, shutdown, tests).
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
- `tools/web/spells.html`: Spell mechanics, progression tiers, and grimoires editor; the engine `effect` of a spell is edited as JSON and summarized in its detail view.

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

The in-process harness (`tests/integration/server_harness.c`) and the command fuzzer share `tests/integration/fixture.h`: game data, an empty world, NPC table and socketpair-backed players. `tests/fuzz/` holds the libFuzzer targets (clang only, see below). The source lists of the server and of the shared code are defined once, in `cmake/DragonGL.cmake` (`DRAGONGL_SERVER_SRCS`, `DRAGONGL_SHARED_SRCS`), and used by the main project, the integration tests and the fuzz targets: a new translation unit is added there.

### Quality checks: `tools/quality.sh`

Every quality gate of CI runs locally with one command; build trees go to `build-quality/`. The clang tools must be version 18, the version CI pins (Ubuntu 24.04).

| Check | What it does | Fails when |
| --- | --- | --- |
| `format` / `format-fix` | clang-format 18 with `.clang-format` on every C file except vendored cJSON | a file is not in the style (`format-fix` rewrites them) |
| `tidy` | clang-tidy 18 with `.clang-tidy` on the main project, the unit and integration tests and the fuzz targets, one process per file | any finding (`WarningsAsErrors: '*'`) |
| `protocol` | `tools/gen_protocol.py --check`: the files generated from `protocol/messages.json` (`include/protocol_gen.h`, `include/wire.h`, `src/shared/wire.c`, `tests/test_protocol_conformance.c`, `tests/protocol_vectors.json`, sections 2-3 of `PROTOCOL.md`) are up to date | a generated file differs from what the table gives |
| `data` | `tools/check_data.py`: `data/schema/*.schema.json` (JSON Schema 2020-12), references resolved by name (book spells, `cast_spell`), unique names, class consistency of books, canonical form (2-space indent, UTF-8) | any error (`tools/check_data.py --fix` rewrites the form) |
| `spelling` | codespell (`.codespellrc`: the game words it would take for typos) on the game data, the documentation, the man pages, the sources and the tools; `LORE.md` is left out, its catalogue cuts texts mid-word | a known misspelling |
| `deps` | `tools/check_deps.py`: the layers of the source tree (see *Server source layout*) and the include graph of the headers | an include that points to a higher layer, a cycle |
| `metrics` | `tools/metrics.py`: function and file length (the server and the shared code also on their own: no function over 120 lines, no file over 1,000), coverage | a value over `tools/quality_limits.json` |
| `coverage` | unit + integration suites built with `-fprofile-instr-generate -fcoverage-mapping`, every process (servers spawned by the network tests included) writes its profile; `llvm-cov` report of `src/server` + `src/shared` in `build-quality/coverage/` (`report.txt`, `html/`) | line coverage under `coverage_line_percent` |
| `tsan` | the integration suite under ThreadSanitizer: the in-process harness and the network tests with a TSan server | any report |
| `fuzz [S]` | the libFuzzer targets of `tests/fuzz` (wire decoders, save-file readers, text commands), S seconds each (default 60), with seeds written by the targets themselves | a crash, leak or timeout; the input goes to `build-quality/fuzz/artifacts/` |
| `all` | all of the above, fuzzing 30 s per target | |

```bash
tools/quality.sh format-fix         # before a commit that touches C code
tools/quality.sh format tidy data   # the static checks
tools/quality.sh coverage metrics   # numbers next to their limits
tools/quality.sh fuzz 300           # a longer fuzzing session
```

`tools/quality_limits.json` is a ratchet: a change may tighten a limit (more coverage, shorter functions), never loosen it. A sanitizer report in a server started by `test_server.py` fails the test that started it (the server log is scanned), so the ASan/UBSan and TSan runs of the network tests are real gates.

A fuzz crash is replayed with the target binary: `build-quality/fuzz/build/fuzz_text_cmd build-quality/fuzz/artifacts/fuzz_text_cmd-crash-…`. Fix it, then add the case to the integration harness or the unit tests so it stays fixed.

### CI

`.github/workflows/ci.yml` runs on every push and pull request:

* Release builds with clang and gcc and `-Werror` (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`);
* the unit tests (Release and ASan/UBSan), the server integration tests under ASan/UBSan, the client smoke test under ASan/UBSan (Xvfb, lavapipe, Vulkan validation layer);
* a server-only build without any graphics package, the RPM build with rpmlint in a Fedora container;
* static analysis: cppcheck and `tools/check_release.py`; clang-tidy; format, game data, spelling, dependencies and metrics;
* coverage (report uploaded as an artifact and summarized on the run page), ThreadSanitizer, fuzzing 60 s per target.

`.github/workflows/fuzz-nightly.yml` fuzzes each target for 20 minutes every night on a corpus kept between runs; `.github/workflows/codeql.yml` runs GitHub code scanning (security-and-quality) on `main` and weekly.

False positives are suppressed where they occur, with the reason: `// cppcheck-suppress <id>` and `// NOLINT(<check>)` / `// NOLINTNEXTLINE(<check>)`. A clang-tidy check that does not fit the code base is disabled in `.clang-tidy`, with a comment that says why.

### Guidelines for Contributions:
1. **Warning-Clean Build:** All C code must compile cleanly with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` using clang and gcc, pass clang-tidy, and be formatted with clang-format (`tools/quality.sh format-fix`).
2. **Data Structure Bounds:** Enum-indexed arrays must be sized using `MAX_*` sentinels to guarantee type safety without bitmask overflow.
3. **Wire and file formats:** a message is changed in one place, its entry in `protocol/messages.json` (fields, ranges, checks, an example), then `tools/gen_protocol.py` regenerates the structs, the codec, the conformance test, the vectors and the message sections of `PROTOCOL.md` (`tools/quality.sh protocol` fails on a stale file). Every wire change increments `version` there, which is also the ALPN protocol `dragongl/<version>`. On-disk formats have their own version numbers in `src/server/persist.h`.
4. **Function size:** within the limits of `tools/quality_limits.json` (`tools/quality.sh metrics`); split by responsibility (see `update_world`, `check_traps`, the item and spell loaders).
5. **Comments say why the code is the way it is now.** The history of a change (what was wrong, which defect it fixed) belongs to `CHANGELOG.md`, not to the source.
6. **Numbers from text** go through `include/strnum.h` (`parse_int`, `parse_ll`, `parse_double`, `str_to_int`, `scan_ints`): they detect overflow and missing digits, where `atoi`/`sscanf` have undefined behavior or silently return 0.
7. **Game data** follows `data/schema/`; a new field is added to the schema in the same change (`tools/quality.sh data`).
