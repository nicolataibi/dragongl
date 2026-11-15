/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The creatures of a new world (see harness_worldgen.h): a whole world is
 * generated as the server does at its first start, then every NPC of
 * every floor is checked.
 */

#include "harness_worldgen.h"

#include "harness_util.h"

#include "ai.h"
#include "bestiary.h"
#include "pathfinding.h"
#include "server_internal.h"
#include "server_startup.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

static bool data_boss(const MonsterTemplate *m) {
    return m->archetype && strcasecmp(m->archetype, "boss") == 0;
}

/* One NPC per tile per floor, during the check. */
static unsigned char g_taken[MAX_FLOORS][MAP_HEIGHT][MAP_WIDTH];

/* Every NPC stands on free open ground inside the border; a creature
 * comes from its floor's range and knows its template by index (what a
 * restart reads); boss floors hold their guardian alone, drawn from the
 * creatures the data calls bosses, which no other floor holds; every
 * fifth floor that is not a boss floor has its wandering merchant; no
 * creature is walled in. */
int test_world_generation(void) {
    /* DGL_WORLD_SEED: another world than the default one of rand(). */
    const char *seed = getenv("DGL_WORLD_SEED");
    if (seed) {
        srand((unsigned int)strtoul(seed, NULL, 10));
    }
    generate_world(&g_game);
    memset(g_taken, 0, sizeof(g_taken));
    int monsters[MAX_FLOORS] = {0}, bosses[MAX_FLOORS] = {0}, others[MAX_FLOORS] = {0},
        merchants[MAX_FLOORS] = {0};
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &g_game.npcs[i];
        if (!n->active || n->floor_id == 0) {
            continue;
        }
        int f = n->floor_id;
        CHECK(f > 0 && f < MAX_FLOORS && n->x >= 1 && n->x < MAP_WIDTH - 1 && n->y >= 1 &&
                  n->y < MAP_HEIGHT - 1,
              "NPC %d out of the map: floor %d (%d,%d)", i, f, n->x, n->y);
        const Map *m = &g_game.world->floors[f].map;
        VoxelType v = m->data[0][n->y][n->x];
        CHECK(voxel_is_ground(v), "NPC %d (archetype %d) on voxel %d, floor %d (%d,%d)", i,
              n->archetype, v, f, n->x, n->y);
        CHECK(!g_taken[f][n->y][n->x]++, "two NPCs on floor %d (%d,%d)", f, n->x, n->y);
        if (n->archetype == ARCH_MERCHANT) {
            merchants[f]++;
            continue;
        }
        if (n->archetype == ARCH_TREASURE || n->archetype == ARCH_GOLD) {
            others[f]++;
            continue;
        }
        CHECK(n->template && n->template_idx >= 0 && n->template_idx < bestiary_size &&
                  n->template == &bestiary_data[n->template_idx],
              "NPC %d: template_idx %d is not its template", i, n->template_idx);
        CHECK(f >= n->template->floor_min && f <= n->template->floor_max,
              "%s (floors %d-%d) on floor %d", n->template->name, n->template->floor_min,
              n->template->floor_max, f);
        CHECK(n->hp > 0 && n->hp == n->max_hp && n->ac > 0 && n->damage_dice > 0,
              "%s on floor %d without its statistics", n->template->name, f);
        /* Walled in: no passable tile in the four directions it walks. */
        bool free_side = false;
        for (int d = 0; d < 4; d++) {
            static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            free_side = free_side || voxel_passable(m->data[0][n->y + dy[d]][n->x + dx[d]]);
        }
        CHECK(free_side, "%s walled in on floor %d (%d,%d)", n->template->name, f, n->x, n->y);
        if (n->archetype == ARCH_BOSS) {
            bosses[f]++;
            CHECK(f % 10 == 0, "boss %s on floor %d", n->template->name, f);
            CHECK(data_boss(n->template), "the guardian of floor %d is %s, not a boss of the data",
                  f, n->template->name);
        } else {
            monsters[f]++;
        }
    }
    for (int f = 1; f < MAX_FLOORS; f++) {
        if (f % 10 == 0) {
            CHECK(bosses[f] == 1 && monsters[f] == 0 && others[f] == 0 && merchants[f] == 0,
                  "boss floor %d: %d bosses, %d monsters, %d piles, %d merchants", f, bosses[f],
                  monsters[f], others[f], merchants[f]);
            continue;
        }
        int wanted = 100 + f * 4 > 300 ? 300 : 100 + f * 4;
        CHECK(monsters[f] == wanted, "floor %d: %d monsters of %d", f, monsters[f], wanted);
        CHECK(others[f] == 15 + 2 * f, "floor %d: %d piles of %d", f, others[f], 15 + 2 * f);
        CHECK(merchants[f] == (f % 5 == 0 ? 1 : 0), "floor %d: %d wandering merchants", f,
              merchants[f]);
    }
    return 0;
}

/* A monster finds its way on the ground of every biome (it walked on
 * stone floor and sand only), and never through a wall when there is no
 * way at all (its fallback step ignored the map). */
int test_monsters_walk_every_biome(void) {
    static const VoxelType k_grounds[] = {VOXEL_WOOD, VOXEL_MUD,    VOXEL_ASH,
                                          VOXEL_ICE,  VOXEL_MARBLE, VOXEL_COBBLE};
    for (size_t g = 0; g < sizeof(k_grounds) / sizeof(k_grounds[0]); g++) {
        hx_reset_world();
        Map *m = &g_game.world->floors[FLOOR].map;
        for (int y = CY - 10; y <= CY + 10; y++) {
            for (int x = CX - 10; x <= CX + 10; x++) {
                m->data[0][y][x] = k_grounds[g];
            }
        }
        PathNode path[MAX_ASTAR_PATH];
        int steps = pathfind_astar(m, CX, CY, CX + 6, CY + 3, path, MAX_ASTAR_PATH);
        CHECK(steps >= 2, "no path on voxel %d", k_grounds[g]);
    }

    /* A wall all around the player: the goblin outside has no way in. */
    hx_reset_world();
    Client *c = hero(0, "walled", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    Map *m = &g_game.world->floors[FLOOR].map;
    for (int d = -2; d <= 2; d++) {
        m->data[0][CY - 2][CX + d] = VOXEL_WALL;
        m->data[0][CY + 2][CX + d] = VOXEL_WALL;
        m->data[0][CY + d][CX - 2] = VOXEL_WALL;
        m->data[0][CY + d][CX + 2] = VOXEL_WALL;
    }
    NPC *n = monster(10, "Goblin", FLOOR, CX + 3, CY);
    for (int t = 0; t < 12; t++) {
        ai_update_npc(&g_game, n, g_test_clients, MAX_CLIENTS, false);
        CHECK(voxel_passable(m->data[0][n->y][n->x]), "the monster walked into a wall at (%d,%d)",
              n->x, n->y);
    }
    return 0;
}

/* dm_spawn: a creature with every statistic of its template, as a new
 * world's (it had hit points only: AC 0, no damage, no experience). */
int test_dm_spawn_full_stats(void) {
    hx_reset_world();
    Client *c = hero(0, "dmspawn", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->is_dm = true;
    int tid = -1;
    for (int i = 0; i < bestiary_size && tid < 0; i++) {
        if (!data_boss(&bestiary_data[i]) && bestiary_data[i].archetype &&
            strcasecmp(bestiary_data[i].archetype, "melee") == 0) {
            tid = i;
        }
    }
    CHECK(tid >= 0, "no melee creature");
    char line[64];
    snprintf(line, sizeof(line), "dm_spawn %d %d %d", tid, CX + 3, CY);
    run(c, line);
    const NPC *n = NULL;
    for (int i = 0; i < MAX_NPCS && !n; i++) {
        if (g_game.npcs[i].active && g_game.npcs[i].x == CX + 3 && g_game.npcs[i].y == CY) {
            n = &g_game.npcs[i];
        }
    }
    const MonsterTemplate *t = &bestiary_data[tid];
    CHECK(n && n->template_idx == tid, "no creature at the tile");
    CHECK(n->hp == t->hp_avg && n->ac == t->ac && n->xp_reward == t->xp &&
              n->damage_dice == t->damage_dice && n->damage_sides == t->damage_sides,
          "dm_spawn: HP %d AC %d XP %d damage %dd%d, template %d/%d/%d/%dd%d", n->hp, n->ac,
          n->xp_reward, n->damage_dice, n->damage_sides, t->hp_avg, t->ac, t->xp, t->damage_dice,
          t->damage_sides);
    /* A mud floor (floors 11-25) is ground too. */
    g_game.world->floors[FLOOR].map.data[0][CY][CX + 5] = VOXEL_MUD;
    snprintf(line, sizeof(line), "dm_spawn %d %d %d", tid, CX + 5, CY);
    run(c, line);
    CHECK(said("Spawned"), "dm_spawn refused a mud floor");
    return 0;
}
