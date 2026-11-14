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
 * npc_index.c - floor stats cache, per-floor index of the active NPCs, entity-id map
 */

#include "server_internal.h"

/*============================================================================
 * Floor Stats Cache — updated incrementally to O(1)
 *
 * Instead of scanning all 50,000 NPCs for each floor on every check
 * density scans (O(N×M)), we maintain per-floor counters updated in O(1)
 * at each NPC event (death, respawn, spawn).
 * ==========================================================================*/
typedef struct {
    int total;  /*Total NPCs allocated on this floor (non-merchants)*/
    int active; /*Active (living) NPCs on this floor*/
} FloorStats;

/*Global cache: one counter per floor + one slot for excluded merchants*/
static FloorStats g_floor_stats[MAX_FLOORS];
static bool g_floor_stats_dirty = true; /* Force cache rebuild on the first tick */

bool floor_stats_get(Game *game, int floor_id, int *total, int *active) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        return false;
    }
    if (g_floor_stats_dirty) {
        floor_stats_rebuild(game);
    }
    *total = g_floor_stats[floor_id].total;
    *active = g_floor_stats[floor_id].active;
    return true;
}

/*Completely rebuilds cache (called only once at startup
* and after a load from file). Cost: O(N) one-off.*/
void floor_stats_rebuild(Game *game) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_stats[f].total = 0;
        g_floor_stats[f].active = 0;
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        if (game->npcs[i].template == NULL) {
            continue;
        }
        if (game->npcs[i].archetype == ARCH_MERCHANT) {
            continue;
        }
        if (game->npcs[i].respawn_timer < 0) {
            continue;
        }
        int f = game->npcs[i].floor_id;
        if (f < 0 || f >= MAX_FLOORS) {
            continue;
        }
        g_floor_stats[f].total++;
        if (game->npcs[i].active) {
            g_floor_stats[f].active++;
        }
    }
    g_floor_stats_dirty = false;
}

/*Update the counter when an NPC dies (decrement active).*/
void floor_stats_npc_died(int floor_id) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        return;
    }
    if (g_floor_stats[floor_id].active > 0) {
        g_floor_stats[floor_id].active--;
    }
}

/*Updates the counter when an NPC respawns or is spawned.*/
void floor_stats_npc_spawned(int floor_id) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        return;
    }
    g_floor_stats[floor_id].active++;
}

/*Single point for NPC alive/dead transitions (see the comment in
 * server_world.h). The stats update mirrors floor_stats_rebuild() EXACTLY
 * (template != NULL, not a merchant, respawn_timer >= 0, floor in range):
 * a slot that the rebuild would not count must not move the counters
 * either, otherwise every summon/drop/corpse-reuse would leak one count
 * per transition. Callers must invoke it with the NPC's FINAL fields
 * already set (template, floor_id, archetype), not before.*/
static void floor_index_add(Game *game, const NPC *n);

static void entity_map_put(int id, int slot, bool replace);

void npc_set_active(Game *game, NPC *n, bool active) {
    if (!n || n->active == active) {
        return;
    }
    n->active = active;
    if (active) {
        floor_index_add(game, n);
        /*Findable by id at once (npc_by_entity_id): the caller set every
         * field before activating.*/
        if (n->entity_id > 0) {
            entity_map_put(n->entity_id, (int)(n - game->npcs), true);
        }
    }
    if (n->archetype == ARCH_MERCHANT) {
        return; /*excluded from the cache, like in floor_stats_rebuild*/
    }
    if (n->template == NULL || n->respawn_timer < 0) {
        return; /*not counted by the rebuild: nothing to keep in sync*/
    }
    if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS) {
        return;
    }
    if (active) {
        floor_stats_npc_spawned(n->floor_id);
    } else {
        floor_stats_npc_died(n->floor_id);
    }
}

/*============================================================================
 * Per-floor active NPC index
 *
 * Local queries (entity broadcast on a step, aggro on an attack, lookups
 * of the commands) walk the active NPCs of one floor instead of all
 * MAX_NPCS (50,000) slots. One sorted array of slots per floor.
 *
 * It is rebuilt every tick and kept sorted by slot; npc_set_active()
 * inserts an NPC that becomes active (every spawn goes through it, with
 * its final floor already set). A death leaves its entry until the
 * rebuild. floor_iter_next() returns the slots in increasing order and
 * checks each one (active, same floor), so a walk sees exactly what a scan
 * of all the slots would see, in the same order.*/
#define FLOOR_INDEX_CAP 4096
static int g_floor_idx[MAX_FLOORS][FLOOR_INDEX_CAP];
static int g_floor_idx_n[MAX_FLOORS];
static bool g_floor_idx_overflow[MAX_FLOORS];

/*First position of a[0..n) whose slot is >= slot.*/
static int lower_bound(const int *a, int n, int slot) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < slot) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void floor_index_add(Game *game, const NPC *n) {
    if (!game->npcs || n < game->npcs || n >= game->npcs + MAX_NPCS) {
        return;
    }
    int slot = (int)(n - game->npcs);
    int f = n->floor_id;
    if (f < 0 || f >= MAX_FLOORS || g_floor_idx_overflow[f]) {
        return;
    }
    int *a = g_floor_idx[f];
    int pos = lower_bound(a, g_floor_idx_n[f], slot);
    if (pos < g_floor_idx_n[f] && a[pos] == slot) {
        return; /*still there: it died and came back since the rebuild*/
    }
    if (g_floor_idx_n[f] >= FLOOR_INDEX_CAP) {
        g_floor_idx_overflow[f] = true; /*walks fall back to the full scan*/
        return;
    }
    memmove(&a[pos + 1], &a[pos], (size_t)(g_floor_idx_n[f] - pos) * sizeof(a[0]));
    a[pos] = slot;
    g_floor_idx_n[f]++;
}

FloorIter floor_iter(Game *game, int floor_id) {
    FloorIter it = {game->npcs, floor_id, -1};
    return it;
}

int floor_iter_next(FloorIter *it) {
    int f = it->floor_id;
    if (f < 0 || f >= MAX_FLOORS) {
        return -1;
    }
    for (;;) {
        int cand;
        if (g_floor_idx_overflow[f]) {
            cand = it->last + 1;
            if (cand >= MAX_NPCS) {
                return -1;
            }
        } else {
            int pos = lower_bound(g_floor_idx[f], g_floor_idx_n[f], it->last + 1);
            if (pos >= g_floor_idx_n[f]) {
                return -1;
            }
            cand = g_floor_idx[f][pos];
        }
        it->last = cand;
        if (it->npcs[cand].active && it->npcs[cand].floor_id == f) {
            return cand;
        }
    }
}

/*Entity id -> NPC slot, rebuilt with the floor index. Open addressing on
 * a table twice the NPC capacity; 0 marks a free key (ids start at 1).
 * Entity ids are unique (next_id only grows and is saved with the NPCs),
 * so a validated hit is the slot a full scan would find.*/
#define ENTITY_MAP_CAP 131072 /*power of two, > 2 * MAX_NPCS*/
static int g_entity_key[ENTITY_MAP_CAP];
static int g_entity_slot[ENTITY_MAP_CAP];

static unsigned entity_hash(int id) {
    return ((unsigned)id * 2654435761u) & (ENTITY_MAP_CAP - 1);
}

static void entity_map_put(int id, int slot, bool replace) {
    unsigned h = entity_hash(id);
    for (int probe = 0; probe < ENTITY_MAP_CAP; probe++, h = (h + 1) & (ENTITY_MAP_CAP - 1)) {
        if (g_entity_key[h] == 0 || g_entity_key[h] == id) {
            if (g_entity_key[h] == 0 || replace) {
                g_entity_key[h] = id;
                g_entity_slot[h] = slot;
            }
            return;
        }
    }
}

static int entity_map_get(int id) {
    unsigned h = entity_hash(id);
    for (int probe = 0; probe < ENTITY_MAP_CAP; probe++, h = (h + 1) & (ENTITY_MAP_CAP - 1)) {
        if (g_entity_key[h] == 0) {
            return -1;
        }
        if (g_entity_key[h] == id) {
            return g_entity_slot[h];
        }
    }
    return -1;
}

NPC *npc_by_entity_id(Game *game, int entity_id) {
    if (entity_id <= 0) {
        return NULL;
    }
    int slot = entity_map_get(entity_id);
    if (slot >= 0 && game->npcs[slot].active && game->npcs[slot].entity_id == entity_id) {
        return &game->npcs[slot];
    }
    /*Not an NPC (the id of a character, of a tombstone), or dead. An NPC is
     * in the map from its activation (npc_set_active) or from the rebuild
     * of the round, so a miss never walks the table.*/
    return NULL;
}

void floor_index_rebuild(Game *game) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_idx_n[f] = 0;
        g_floor_idx_overflow[f] = false;
    }
    memset(g_entity_key, 0, sizeof(g_entity_key));
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &game->npcs[i];
        if (!n->active) {
            continue;
        }
        if (n->entity_id > 0) {
            entity_map_put(n->entity_id, i, false); /*lowest slot wins, as in a scan*/
        }
        int f = n->floor_id;
        if (f < 0 || f >= MAX_FLOORS) {
            continue;
        }
        if (g_floor_idx_overflow[f]) {
            continue;
        }
        if (g_floor_idx_n[f] >= FLOOR_INDEX_CAP) {
            /* Pathologically crowded floor: walks scan every slot. */
            g_floor_idx_overflow[f] = true;
            continue;
        }
        g_floor_idx[f][g_floor_idx_n[f]++] = i;
    }
}

/*Returns the index of the floor's active NPCs, or NULL when the floor
 * overflowed the cap (the caller must then do the full scan).*/
const int *floor_index_for(int floor_id, int *count_out) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        if (count_out) {
            *count_out = 0;
        }
        return NULL;
    }
    if (g_floor_idx_overflow[floor_id]) {
        if (count_out) {
            *count_out = -1;
        }
        return NULL;
    }
    if (count_out) {
        *count_out = g_floor_idx_n[floor_id];
    }
    return g_floor_idx[floor_id];
}
