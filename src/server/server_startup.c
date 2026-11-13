/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * server_startup.c - Start of the server: the data directory, the game data, the world.
 */

#include "server_startup.h"
#include "data_loader.h"
#include "dragongl_config.h"
#include "login_worker.h"
#include <sys/random.h>
#include <unistd.h>

#include "ai.h"
#include "server_internal.h"
#include "server_spawn.h"

#include "server_log.h"

/*Seed the RNG once at process start, with kernel entropy when available:
 * a world loaded from disk skips world_init(), and must not replay the
 * default seed after every restart.*/
void seed_game_rng(void) {
    uint32_t seed = 0;
    if (getrandom(&seed, sizeof(seed), 0) != sizeof(seed)) {
        /*Fallback: mix time, pid and a stack address.*/
        seed = (uint32_t)time(NULL) * 2654435761u;
        seed ^= (uint32_t)getpid() * 40503u;
        seed ^= (uint32_t)(uintptr_t)&seed;
    }
    srand(seed);
    /*The seed is NOT logged: with it, anyone reading the log could
     * replay every die roll and loot drop of rand().*/
}

static bool has_bestiary(const char *dir) {
    char probe[DATA_DIR_MAX + 32];
    return snprintf(probe, sizeof(probe), "%s/bestiary.json", dir) < (int)sizeof(probe) &&
           access(probe, R_OK) == 0;
}

/*Data directory (read-only JSON). In order: --data-dir (taken as given),
 * $DRAGONGL_DATA_DIR, the data/ of the source tree when the binary runs
 * from a build directory (<exe>/../data), the installed
 * DRAGONGL_PKGDATADIR/data. The current directory is not searched.*/
void resolve_data_dir(Game *game) {
    if (game->data_dir[0]) {
        return;
    }
    const char *env = getenv("DRAGONGL_DATA_DIR");
    if (env && env[0]) {
        copy_str(game->data_dir, env, sizeof(game->data_dir));
        return;
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char cand[sizeof(exe) + 16];
            snprintf(cand, sizeof(cand), "%s/../data", exe);
            char real[4096];
            if (strlen(cand) < sizeof(real) && realpath(cand, real) && has_bestiary(real) &&
                strlen(real) < sizeof(game->data_dir)) {
                copy_str(game->data_dir, real, sizeof(game->data_dir));
                return;
            }
        }
    }
    copy_str(game->data_dir, DRAGONGL_PKGDATADIR "/data", sizeof(game->data_dir));
}

/*Returns 0, or the exit status: 2 without game data, 1 for unreadable data.*/
int load_game_data(Game *game) {
    char probe[DATA_DIR_MAX + 32];
    snprintf(probe, sizeof(probe), "%s/bestiary.json", game->data_dir);
    if (access(probe, R_OK) != 0) {
        server_log("SYS",
                   "ERROR: no game data: %s is missing. Use --data-dir <dir> or "
                   "install the data package.",
                   probe);
        return 2; /*configuration error*/
    }
    if (!init_data_loaders(game->data_dir)) {
        server_log("SYS", "ERROR: the game data in %s cannot be loaded (see above).",
                   game->data_dir);
        return 1;
    }
    return 0;
}

/*world.dat and npcs.dat are a pair: both load, or a new world is generated
 * and saved (never a partial one). False if the new world cannot be saved.*/
bool load_or_generate_world(Game *game) {
    for (int i = 0; i < MAX_NPCS; i++) {
        game->npcs[i].active = false;
        game->npcs[i].archetype = ARCH_MELEE;
        game->npcs[i].effect_count = 0;
        game->npcs[i].respawn_timer = -1;
    }
    char world_path[STATE_DIR_MAX + 32], npcs_path[STATE_DIR_MAX + 32], why[96];
    bool world_ok = false, npcs_ok = false;
    if (state_path(world_path, sizeof(world_path), "world.dat") &&
        state_path(npcs_path, sizeof(npcs_path), "npcs.dat")) {
        BinFileStatus st = persist_read_world(world_path, game->world, why, sizeof(why));
        world_ok = (st == BINFILE_OK);
        if (st == BINFILE_INVALID) {
            server_log("SYS", "WARNING: %s unreadable (%s).", world_path, why);
        }
        if (world_ok) {
            int loaded = 0;
            st = persist_read_npcs(npcs_path, game->npcs, MAX_NPCS, &game->next_id,
                                   &game->total_turns, &loaded, why, sizeof(why));
            npcs_ok = (st == BINFILE_OK);
            if (npcs_ok) {
                server_log("SYS", "World and %d NPCs loaded from %s.", loaded, persist_state_dir());
            } else if (st == BINFILE_INVALID) {
                server_log("SYS", "WARNING: %s unreadable (%s).", npcs_path, why);
            }
        }
    }

    if (world_ok && npcs_ok) {
        /*Re-bind what is not stored: the template pointer (from template_idx)
     * and the behavior derived from the archetype.*/
        for (int i = 0; i < MAX_NPCS; i++) {
            if (game->npcs[i].active || game->npcs[i].respawn_timer >= 0) {
                if (game->npcs[i].archetype == ARCH_TREASURE ||
                    game->npcs[i].archetype == ARCH_GOLD) {
                    game->npcs[i].template = NULL;
                } else {
                    int tidx = game->npcs[i].template_idx;
                    if (tidx < 0 || tidx >= bestiary_size) {
                        tidx = 0;
                    }
                    game->npcs[i].template = &bestiary_data[tidx];
                    ai_attach_behavior(&game->npcs[i]);
                }
            }
        }
        return true;
    }
    /*Start from a clean slate: a half-read npcs.dat may have filled slots.*/
    memset(game->npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int i = 0; i < MAX_NPCS; i++) {
        game->npcs[i].respawn_timer = -1;
    }
    game->next_id = 1;
    game->total_turns = 0;
    server_log("SYS", "Generating a new world in %s ...", persist_state_dir());
    world_init(game->world);
    spawn_city_merchants(game);
    spawn_magic_shops(game);
    spawn_martial_archive(game);
    populate_dungeons(game);
    if (!save_world_state(game)) {
        server_log("SYS", "ERROR: the new world could not be saved in %s.", persist_state_dir());
        return false;
    }
    server_log("SYS", "New world generated and saved.");
    return true;
}
