/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Fuzz target: the text commands (handle_text_cmd and every handler of
 * the command table).
 *
 * An input is a MSG_TEXT_CMD payload, decoded and cleaned as the server
 * does it (main_server.c, handle_text_message: control characters become
 * spaces), then run for a level-1 player in the city (floor 0), or for
 * the DM when the line starts with "dm_". The world is the city and the
 * first dungeon floor, generated with a fixed seed, with the merchants,
 * shops and archive of a new server; floors 2 and deeper are empty. The
 * player, the two floors, the NPCs and the RNG are restored before every
 * input, so an input behaves the same alone or in a run.
 */

#include "fuzz_common.h"
#include "fixture.h"

#include "server_cmd.h"
#include "server_commands.h"
#include "server_conn.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "server_world.h"
#include "wire.h"

#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_FLOORS 2 /* floors generated: the city and the first dungeon */

/* The state every input starts from. */
static Client g_template; /* the player as fx_player() made it */
static Floor *g_floors;   /* floors 0..FUZZ_FLOORS-1 */
static NPC *g_city_npcs;  /* slots 0..g_city_slots-1: merchants, shops, archive */
static int g_city_slots;
static int g_next_id;

static void seed_line(const char *dir, int *count, const char *line) {
    BinWriter w;
    bw_init(&w);
    if (wire_put_text_cmd(&w, line) && w.len > WIRE_HEADER_SIZE) {
        char name[32];
        snprintf(name, sizeof(name), "cmd-%03d", (*count)++);
        fuzz_write_seed(dir, name, -1, w.data + WIRE_HEADER_SIZE, w.len - WIRE_HEADER_SIZE);
    }
    bw_free(&w);
}

/* One seed per command word of the table, bare and with arguments. */
static void write_seeds(const char *dir) {
    int count = 0;
    size_t rows = 0;
    const CmdEntry *t = cmd_commands(&rows);
    for (size_t i = 0; i < rows; i++) {
        const char *lists[2] = {t[i].names, t[i].prefixes};
        for (int l = 0; l < 2; l++) {
            const char *p = lists[l];
            while (p && *p) {
                const char *bar = strchr(p, '|');
                size_t len = bar ? (size_t)(bar - p) : strlen(p);
                char word[64];
                snprintf(word, sizeof(word), "%.*s", (int)len, p);
                seed_line(dir, &count, word);
                char line[96];
                snprintf(line, sizeof(line), "%s 1 2", word);
                seed_line(dir, &count, line);
                p = bar ? bar + 1 : NULL;
            }
        }
    }
    static const char *const dm[] = {"dm_gold 100", "dm_level 5",      "dm_goto 3 60 60",
                                     "dm_spawn 1",  "dm_shop 0",       "dm_find_item Dagger",
                                     "dm_boss 2",   "dm_merchant 1 2", "dm_telemetry"};
    for (size_t i = 0; i < sizeof(dm) / sizeof(dm[0]); i++) {
        seed_line(dir, &count, dm[i]);
    }
}

/* The signature is libFuzzer's: argc and argv may be changed by the target. */
// NOLINTNEXTLINE(readability-non-const-parameter)
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    const char *dir = fuzz_seed_dir();
    if (dir) {
        write_seeds(dir); /* the table is static: no fixture needed */
        exit(0);
    }
    char state[300];
    snprintf(state, sizeof(state), "%s/state", fuzz_tmp_dir());
    if (fx_init(state, DGL_FUZZ_DATA_DIR) != 0) {
        exit(1);
    }
    Client *c = fx_player(0, "fuzzer");
    if (!c) {
        exit(1);
    }
    g_template = *c;

    srand(1);
    for (int f = 0; f < FUZZ_FLOORS; f++) {
        floor_init(&master_world->floors[f], f);
    }
    next_id = 1;
    spawn_city_merchants(g_npcs, &next_id);
    spawn_magic_shops(g_npcs, &next_id);
    spawn_martial_archive(g_npcs, &next_id);
    g_next_id = next_id;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_npcs[i].active || g_npcs[i].respawn_timer >= 0) {
            g_city_slots = i + 1;
        }
    }
    g_floors = malloc(sizeof(Floor) * FUZZ_FLOORS);
    g_city_npcs = malloc(sizeof(NPC) * (size_t)(g_city_slots > 0 ? g_city_slots : 1));
    if (!g_floors || !g_city_npcs) {
        exit(1);
    }
    memcpy(g_floors, master_world->floors, sizeof(Floor) * FUZZ_FLOORS);
    memcpy(g_city_npcs, g_npcs, sizeof(NPC) * (size_t)g_city_slots);
    floor_index_rebuild(g_npcs);
    floor_stats_rebuild(g_npcs);
    return 0;
}

static void restore_player(void) {
    Client *c = &g_test_clients[0];
    fx_drain(0);
    if (!conn_is_open(0) || conn_dropping(0)) {
        fx_release(0);
        if (!fx_reconnect(0)) {
            abort();
        }
    }
    int sock = c->sock;
    *c = g_template;
    c->sock = sock;
}

/* Back to the world of LLVMFuzzerInitialize: floors, NPCs, ids. Floors
 * 2 and deeper were never generated and only change through the DM. */
static void restore_world(void) {
    memcpy(master_world->floors, g_floors, sizeof(Floor) * FUZZ_FLOORS);
    memcpy(g_npcs, g_city_npcs, sizeof(NPC) * (size_t)g_city_slots);
    for (int i = g_city_slots; i < MAX_NPCS; i++) {
        if (g_npcs[i].active || g_npcs[i].respawn_timer >= 0) {
            memset(&g_npcs[i], 0, sizeof(g_npcs[i]));
            g_npcs[i].respawn_timer = -1;
        }
    }
    next_id = g_next_id;
    floor_index_rebuild(g_npcs);
    floor_stats_rebuild(g_npcs);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    MsgTextCmd tc;
    if (size > WIRE_MAX_CLIENT_PAYLOAD || !wire_get_text_cmd(data, size, &tc)) {
        return 0;
    }
    for (char *p = tc.cmd; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7F) {
            *p = ' ';
        }
    }
    restore_world();
    restore_player();
    Client *c = &g_test_clients[0];
    c->is_dm = strncmp(tc.cmd, "dm_", 3) == 0;
    srand(1);
    handle_text_cmd(c, tc.cmd, g_npcs);
    fx_drain(0);
    return 0;
}
