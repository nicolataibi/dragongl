/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 *
 * Benchmark of the game round, against the thresholds of
 * tests/bench_limits.json.
 *
 * A full world is generated (the city, every dungeon floor and its
 * population) and MAX_CLIENTS characters are put in the dungeon. Every
 * round each of them takes a step, the world advances and what the server
 * queued is written to the sockets: that is what is timed. Two layouts:
 * "spread", one character per floor (the most floors awake), and "crowd",
 * all of them on one floor (the most characters seeing each other). The
 * sockets are socketpairs: TLS and the kernel network path are not part
 * of this number (tools/loadtest.py measures the whole server).
 *
 *   tick_bench <limits.json> [rounds] [json output]
 */

#include "fixture.h"

#include "cJSON.h"
#include "map.h"
#include "server_conn.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_metrics.h"
#include "server_move.h"
#include "server_startup.h"
#include "server_tiles.h"
#include "strnum.h"
#include "server_world.h"
#include "writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_ROUNDS 600

typedef struct {
    const char *name;
    bool crowd;
    MetricSummary result;
} Scenario;

static int active_npcs(void) {
    int n = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        n += g_game.npcs[i].active;
    }
    return n;
}

/*A free tile of the floor, searched from a pseudo-random point.*/
static bool free_tile(const Floor *fl, int *x, int *y) {
    for (int tries = 0; tries < 20000; tries++) {
        int tx = 1 + rand() % (MAP_WIDTH - 2);
        int ty = 1 + rand() % (MAP_HEIGHT - 2);
        if (tile_free(fl, tx, ty)) {
            *x = tx;
            *y = ty;
            return true;
        }
    }
    return false;
}

/*Puts character i on a floor: its own one (spread) or floor 1, near the
 * others (crowd).*/
static bool place(Client *c, int i, bool crowd, int crowd_x, int crowd_y) {
    Floor *old = &g_game.world->floors[c->floor_id];
    if (old->entity_grid[c->y][c->x] == c->entity_id) {
        old->entity_grid[c->y][c->x] = 0;
    }
    int floor_id = crowd ? 1 : 1 + i % (MAX_FLOORS - 1);
    Floor *fl = &g_game.world->floors[floor_id];
    int x = 0, y = 0;
    if (crowd) {
        find_walkable_near(fl, crowd_x + (i % 8) * 2 - 8, crowd_y + (i / 8) * 2 - 8, &x, &y);
    } else if (!free_tile(fl, &x, &y)) {
        return false;
    }
    c->floor_id = floor_id;
    c->x = x;
    c->y = y;
    fl->entity_grid[y][x] = c->entity_id;
    return true;
}

static bool setup(bool crowd) {
    srand(20261114);
    generate_world(&g_game);
    floor_index_rebuild(&g_game);
    int cx = 0, cy = 0;
    if (!free_tile(&g_game.world->floors[1], &cx, &cy)) {
        return false;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g_test_clients[i];
        c->level = 10;
        c->str = c->dex = c->con = c->intel = c->wis = c->cha = 14;
        c->floor_id = 0;
        c->x = c->y = 0;
        if (!place(c, i, crowd, cx, cy)) {
            return false;
        }
    }
    return true;
}

/*One round of the server: input, simulation, output.*/
static void one_round(void) {
    static const int step[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g_test_clients[i];
        /*The benchmark needs them alive and in the dungeon to the end.*/
        c->hp = c->max_hp = 100000;
        c->hunger_level = 0;
        c->exhaustion_level = 0;
        c->last_action_ms = 0; /*no step is refused as too early*/
        int d = rand() % 4;
        MsgMove m = {step[d][0], step[d][1]};
        player_move(c, &m, g_game.clients, &g_game);
    }
    update_world(g_game.clients, &g_game);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (conn_tx_pending(i)) {
            conn_flush(i);
        }
    }
}

static bool run(Scenario *s, int rounds) {
    if (!setup(s->crowd)) {
        fprintf(stderr, "tick_bench: cannot place the characters (%s)\n", s->name);
        return false;
    }
    static MetricHistogram h;
    memset(&h, 0, sizeof(h));
    int in_dungeon = 0;
    for (int r = 0; r < rounds + 20; r++) {
        uint64_t t0 = metrics_now_us();
        one_round();
        uint64_t dt = metrics_now_us() - t0;
        if (r >= 20) { /*the first rounds wake the floors up*/
            histogram_add(&h, dt, 0);
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            fx_drain(i);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        in_dungeon += g_test_clients[i].active && g_test_clients[i].floor_id > 0;
    }
    histogram_summary(&h, &s->result);
    printf("%-7s %5llu rounds  mean %7.2f  p50 %7.2f  p99 %7.2f  max %7.2f ms"
           "   (%d NPCs alive, %d/%d characters in the dungeon)\n",
           s->name, (unsigned long long)s->result.count, s->result.mean_ms, s->result.p50_ms,
           s->result.p99_ms, s->result.max_ms, active_npcs(), in_dungeon, MAX_CLIENTS);
    if (in_dungeon < MAX_CLIENTS) {
        fprintf(stderr,
                "tick_bench: %s: characters left the dungeon, the load is not the one "
                "that was asked\n",
                s->name);
        return false;
    }
    return true;
}

/*What a round that autosaves adds: one step of the autosave, serialized
 * on the game thread and handed to the writer thread (the disk is not
 * part of the number). Worst step of `times` autosaves.*/
static double autosave_ms(int times, double step_ms[AUTOSAVE_STEPS]) {
    uint64_t worst[AUTOSAVE_STEPS] = {0};
    if (!writer_start()) {
        return -1.0;
    }
    for (int k = 0; k < times; k++) {
        for (int step = 0; step < AUTOSAVE_STEPS; step++) {
            uint64_t t0 = metrics_now_us();
            bool ok = autosave_step(g_game.clients, &g_game, step);
            uint64_t dt = metrics_now_us() - t0;
            if (!ok) {
                (void)writer_stop();
                return -1.0;
            }
            worst[step] = dt > worst[step] ? dt : worst[step];
        }
    }
    if (!writer_stop() || writer_failures() != 0) {
        return -1.0;
    }
    double longest = 0.0;
    for (int step = 0; step < AUTOSAVE_STEPS; step++) {
        step_ms[step] = (double)worst[step] / 1000.0;
        longest = step_ms[step] > longest ? step_ms[step] : longest;
    }
    return longest;
}

/*limits.json: {"tick_p99_ms": {"spread": 50, "crowd": 50},
 * "autosave_ms": {"max": 50}}.*/
static bool limit_of(const char *path, const char *group, const char *scenario, double *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(root, group), scenario);
    bool ok = cJSON_IsNumber(v);
    if (ok) {
        *out = v->valuedouble;
    }
    cJSON_Delete(root);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tick_bench <limits.json> [rounds] [json output]\n");
        return 2;
    }
    int rounds = argc > 2 ? str_to_int(argv[2]) : DEFAULT_ROUNDS;
    if (rounds < 100) {
        rounds = 100;
    }
    if (fx_init("state-tick-bench", DGL_TEST_DATA_DIR) != 0) {
        return 2;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "bench%02d", i);
        if (!fx_player(i, name)) {
            fprintf(stderr, "tick_bench: cannot create %s\n", name);
            return 2;
        }
    }
    Scenario scenarios[] = {{"spread", false, {0}}, {"crowd", true, {0}}};
    int failed = 0;
    FILE *json = argc > 3 ? fopen(argv[3], "w") : NULL;
    if (json) {
        fprintf(json, "{\n");
    }
    for (size_t k = 0; k < sizeof(scenarios) / sizeof(scenarios[0]); k++) {
        Scenario *s = &scenarios[k];
        if (!run(s, rounds)) {
            return 2;
        }
        double limit = 0.0;
        if (!limit_of(argv[1], "tick_p99_ms", s->name, &limit)) {
            fprintf(stderr, "tick_bench: no tick_p99_ms.%s in %s\n", s->name, argv[1]);
            return 2;
        }
        if (s->result.p99_ms > limit) {
            fprintf(stderr, "tick_bench: FAIL %s: p99 %.2f ms over the limit of %.2f ms\n", s->name,
                    s->result.p99_ms, limit);
            failed++;
        }
        if (json) {
            fprintf(json, "  \"%s\": {\"p50_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f}%s\n",
                    s->name, s->result.p50_ms, s->result.p99_ms, s->result.max_ms, ",");
        }
    }
    double save_limit = 0.0;
    double step_ms[AUTOSAVE_STEPS] = {0};
    double save_ms = autosave_ms(5, step_ms);
    if (save_ms < 0.0 || !limit_of(argv[1], "autosave_ms", "max", &save_limit)) {
        fprintf(stderr, "tick_bench: the autosave failed, or no autosave_ms.max in %s\n", argv[1]);
        return 2;
    }
    printf("autosave on the game thread: characters and world %.2f ms, NPC table %.2f ms\n",
           step_ms[0], step_ms[1]);
    if (save_ms > save_limit) {
        fprintf(stderr, "tick_bench: FAIL autosave: %.2f ms over the limit of %.2f ms\n", save_ms,
                save_limit);
        failed++;
    }
    if (json) {
        fprintf(json, "  \"autosave\": {\"max_ms\": %.3f}\n", save_ms);
        fprintf(json, "}\n");
        fclose(json);
    }
    return failed ? 1 : 0;
}
