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

/**
 * server_world.c — World tick, NPC respawn and density monitor
 *
 * Contains the world update logic executed on every iteration
 * of the server's main loop:
 *   - Periodic auto-save of players
 *   - Light source and hunger resource consumption
 *   - Periodic effects and conditions (poison, fire, bleeding)
 *   - Saving throws for condition recovery (players and NPCs)
 *   - Speed/Tick System for NPCs (energy and actions)
 *   - NPC respawn with countdown
 *   - Density Monitor: emergency respawn if a floor is too empty
 *   - Persistent AoE cloud updates
 */

#include "server_world.h"
#include "persist.h"
#include <errno.h>
#include "server_internal.h"
#include "ai.h"
#include "aoe.h"
#include "combat_log.h"
#include "rules.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*Energy threshold for NPC action in the Speed/Tick System*/
#define ENERGY_THRESHOLD 10

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

/*Completely rebuilds cache (called only once at startup
* and after a load from file). Cost: O(N) one-off.*/
void floor_stats_rebuild(NPC *npcs) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_stats[f].total = 0;
        g_floor_stats[f].active = 0;
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].template == NULL) {
            continue;
        }
        if (npcs[i].archetype == ARCH_MERCHANT) {
            continue;
        }
        if (npcs[i].respawn_timer < 0) {
            continue;
        }
        int f = npcs[i].floor_id;
        if (f < 0 || f >= MAX_FLOORS) {
            continue;
        }
        g_floor_stats[f].total++;
        if (npcs[i].active) {
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
static void floor_index_add(const NPC *n);

void npc_set_active(NPC *n, bool active) {
    if (!n || n->active == active) {
        return;
    }
    n->active = active;
    if (active) {
        floor_index_add(n);
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

static void floor_index_add(const NPC *n) {
    if (!g_npcs || n < g_npcs || n >= g_npcs + MAX_NPCS) {
        return;
    }
    int slot = (int)(n - g_npcs);
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

FloorIter floor_iter(const NPC *npcs, int floor_id) {
    FloorIter it = {npcs, floor_id, -1};
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

NPC *npc_by_entity_id(NPC *npcs, int entity_id) {
    if (entity_id <= 0) {
        return NULL;
    }
    int slot = entity_map_get(entity_id);
    if (slot >= 0 && npcs[slot].active && npcs[slot].entity_id == entity_id) {
        return &npcs[slot];
    }
    /*Spawned since the last rebuild: scan once and remember it.*/
    for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active && npcs[i].entity_id == entity_id) {
            entity_map_put(entity_id, i, true);
            return &npcs[i];
        }
    }
    return NULL;
}

void floor_index_rebuild(const NPC *npcs) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_idx_n[f] = 0;
        g_floor_idx_overflow[f] = false;
    }
    memset(g_entity_key, 0, sizeof(g_entity_key));
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &npcs[i];
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

/*============================================================================
 * update_world — Major world state update
 *
 * Each iteration of the server's main loop must be called.
 * @clients: Array of connected clients.
 * @npcs: Array of active and inactive NPCs.
 * ==========================================================================*/

/*Respawn a dead player in town: frees the cell on the old floor, tells
 * that floor the player left, registers the player in the town grid and
 * sends the client its new state and the town map. Every death site uses
 * it.*/
void player_respawn_town(Client *c) {
    if (!c || !master_world) {
        return;
    }
    int old_floor = c->floor_id;
    if (old_floor >= 0 && old_floor < MAX_FLOORS && c->x >= 0 && c->x < MAP_WIDTH && c->y >= 0 &&
        c->y < MAP_HEIGHT &&
        master_world->floors[old_floor].entity_grid[c->y][c->x] == c->entity_id) {
        master_world->floors[old_floor].entity_grid[c->y][c->x] = 0;
    }
    c->hp = c->max_hp;
    c->floor_id = 0;
    c->x = MAP_CENTER_X + 1;
    c->y = MAP_CENTER_Y + 1;
    if (master_world->floors[0].entity_grid[c->y][c->x] == 0) {
        master_world->floors[0].entity_grid[c->y][c->x] = c->entity_id;
    }
    notify_player_left_floor(c, old_floor);
    broadcast_player_state(c);
    send_detailed_state(c);
    send_map_chunk(c->sock, &master_world->floors[0].map, c->x, c->y, INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, g_npcs);
}

static void broadcast_game_time(Client *clients) {
    int total_mins = (global_total_turns % 1440);
    int h = (total_mins / 60 + 8) % 24;
    int m = total_mins % 60;

    MsgTimeSync ts;
    ts.game_hour = h;
    ts.game_min = m;
    ts.total_turns = global_total_turns;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated) {
            send_time_sync(clients[i].sock, &ts);
        }
    }
}

/*Snapshot world.dat and npcs.dat (in this order) for the writer thread,
 * or write them synchronously when it is not running (startup, shutdown).
 * Returns false if either could not be written (the writer logs why); the
 * previous file is kept in that case.*/
bool save_world_state(const NPC *npcs) {
    char world_path[STATE_DIR_MAX + 32], npcs_path[STATE_DIR_MAX + 32];
    if (!state_path(world_path, sizeof(world_path), "world.dat") ||
        !state_path(npcs_path, sizeof(npcs_path), "npcs.dat")) {
        server_log("SAVE", "State directory path too long.");
        return false;
    }
    return persist_write_world(world_path, master_world) &&
           persist_write_npcs(npcs_path, npcs, MAX_NPCS, next_id, global_total_turns);
}

/*Every character, the world and the NPCs, once a minute.*/
static void autosave_if_due(Client *clients, NPC *npcs) {
    static time_t last_autosave = 0;
    time_t now = time(NULL);
    if (last_autosave == 0) {
        last_autosave = now;
    }
    if (now - last_autosave >= 60) {
        last_autosave = now;
        int saved_count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].authenticated) {
                save_player_data(&clients[i]);
                saved_count++;
            }
        }

        if (master_world) {
            if (save_world_state(npcs)) {
                server_log("SYS", "Auto-save: %d players, world and NPCs.", saved_count);
            }
        }
    }
}

/*The global invasion event: starts every 1500 rounds on a random floor
 * 1-10, ends with a reward when its goal is met or when time runs out.*/
static void update_global_event(Client *clients) {
    if (active_event_type == 0) {
        if (global_total_turns % 1500 == 0) {   //Every ~300 seconds (5 min real at 5 tick/s)
            active_event_type = 1;              // 1 = Skeletons
            event_floor_id = (rand() % 10) + 1; //Floor 1-10
            event_time_left = 1500;             // 5 minutes
            event_progress = 0;
            event_goal = 5; // 5 Skeletons

            //Broadcast to everyone
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] Invasion on floor %d! Kill %d Skeletons! Time: 5 minutes.",
                        event_floor_id, event_goal);
                }
            }
        }
    } else {
        event_time_left--;
        if (event_progress >= event_goal) {
            // Victory
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] The invasion has been stopped! Defenders are rewarded.");
                    if (clients[j].floor_id == event_floor_id) {
                        clients[j].gold += 500;
                        send_text_to_client(clients[j].sock,
                                            "[SYSTEM] You received 500 gold for your bravery!");
                    }
                }
            }
            active_event_type = 0;
        } else if (event_time_left <= 0) {
            // Defeat
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] Time is up, the monsters retreat into the shadows...");
                }
            }
            active_event_type = 0;
        }
    }
}

/*One round of a player in the dungeon (floor > 0): light sources, hunger,
 * conditions and their saving throws, periodic damage.*/
static void player_round(Client *clients, int i) {
    if (!clients[i].active || !clients[i].authenticated) {
        return;
    }
    if (clients[i].floor_id <= 0) {
        return;
    }

    /*--- Light source consumption ---*/
    ItemInstance *ls[] = {&clients[i].slot_hand_r, &clients[i].slot_hand_l, &clients[i].belt[0],
                          &clients[i].belt[1],     &clients[i].belt[2],     &clients[i].belt[3]};
    for (int j = 0; j < 6; j++) {
        if (ls[j]->template_idx == -1) {
            continue;
        }
        const ItemTemplate *it = &item_database[ls[j]->template_idx];
        if (it->category != ITEM_LIGHT_SOURCE) {
            continue;
        }
        if (ls[j]->durability > 0) {
            ls[j]->durability--;
            if (ls[j]->durability == 0) {
                send_text_to_client(clients[i].sock, "[SYSTEM] Your light source has gone out!");
            }
        }
    }

    /* --- Hunger and Exhaustion System --- */
    clients[i].hunger_level++;
    if (clients[i].hunger_level >= HUNGER_MAX) {
        if (global_total_turns % 2 == 0) {
            clients[i].hp -= 2;
            send_text_to_client(clients[i].sock, "[SYSTEM] You're starving!");
            if (clients[i].hp <= 0) {
                server_log("DEATH", "%s starved to death.", clients[i].username);
                save_bones(&clients[i]);
                player_respawn_town(&clients[i]);
                send_text_to_client(
                    clients[i].sock,
                    "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
            }
        }
        /*Exhaustion accumulates while you stay hungry*/
        if (global_total_turns % 100 == 0) {
            clients[i].exhaustion_level++;
            send_text_to_client(clients[i].sock, "[SYSTEM] You have gained a level of Exhaustion!");
        }
    }

    /* --- Update effects and conditions --- */
    if (rules_update_effects(clients[i].effects, &clients[i].effect_count)) {
        send_detailed_state(&clients[i]);
    }

    /*--- Saving throws to recover player condition ---*/
    const char *conditions_to_check[] = {"Paralyzed", "Stunned",   "Unconscious", "Burning",
                                         "Bleeding",  "Petrified", "Frozen"};
    for (int c_idx = 0; c_idx < 7; c_idx++) {
        if (!rules_has_condition(clients[i].effects, clients[i].effect_count,
                                 conditions_to_check[c_idx])) {
            continue;
        }
        int ts, td, tc, ti, tw, th;
        get_total_stats(&clients[i], &ts, &td, &tc, &ti, &tw, &th);

        /*Attribute used for the saving throw varies by condition*/
        int mod = 0;
        if (strcmp(conditions_to_check[c_idx], "Burning") == 0) {
            mod = rules_get_modifier(td); /*Dexterity: flame reflexes*/
        } else if (strcmp(conditions_to_check[c_idx], "Petrified") == 0 ||
                   strcmp(conditions_to_check[c_idx], "Frozen") == 0) {
            mod = rules_get_modifier(ts); /*Strength: break the stone or the ice*/
        } else {
            mod = rules_get_modifier(tc); /*Constitution: bleeding and the rest*/
        }

        /*Disadvantage on saving throws if cursed*/
        bool dis = rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_CURSED);

        int roll_v = 0;
        bool success = rules_roll_save(mod, 12, false, dis, &roll_v);
        clog_save(clients[i].username, conditions_to_check[c_idx], roll_v, mod, 12, success);

        if (success) {
            for (int e_idx = 0; e_idx < clients[i].effect_count; e_idx++) {
                if (strcasecmp(clients[i].effects[e_idx].name, conditions_to_check[c_idx]) == 0) {
                    clients[i].effects[e_idx] = clients[i].effects[clients[i].effect_count - 1];
                    clients[i].effect_count--;
                    send_text_to_client(clients[i].sock,
                                        "[SYSTEM] You have recovered from the %s state!",
                                        conditions_to_check[c_idx]);
                    send_detailed_state(&clients[i]);
                    break;
                }
            }
        }
    }

    /* --- Invisibility notification (every 10 rounds) --- */
    if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_INVISIBLE)) {
        if (global_total_turns % 10 == 0) {
            send_text_to_client(clients[i].sock, "[SYSTEM] You are currently invisible.");
        }
    }

    /*--- Player condition periodic effects ---*/
    if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_POISONED)) {
        clients[i].hp -= 1;
        send_text_to_client(clients[i].sock,
                            "[SYSTEM] Feel the poison coursing through your veins...");
        if (clients[i].hp <= 0) {
            server_log("DEATH", "%s died from poison.", clients[i].username);
            save_bones(&clients[i]);
            player_respawn_town(&clients[i]);
            send_text_to_client(
                clients[i].sock,
                "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
        send_detailed_state(&clients[i]);
    }

    if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_BURNING)) {
        /*If in water, turn off immediately*/
        Floor *fl = &master_world->floors[clients[i].floor_id];
        if (fl->map.data[0][clients[i].y][clients[i].x] == VOXEL_WATER) {
            for (int e_idx = 0; e_idx < clients[i].effect_count; e_idx++) {
                if (strcasecmp(clients[i].effects[e_idx].name, "Burning") == 0) {
                    clients[i].effects[e_idx] = clients[i].effects[clients[i].effect_count - 1];
                    clients[i].effect_count--;
                    send_text_to_client(clients[i].sock, "[SYSTEM] Water extinguishes flames!");
                    send_detailed_state(&clients[i]);
                    break;
                }
            }
        } else {
            clients[i].hp -= 2;
            send_text_to_client(clients[i].sock, "[SYSTEM] You are burning!");
            if (clients[i].hp <= 0) {
                server_log("DEATH", "%s burned to death.", clients[i].username);
                save_bones(&clients[i]);
                player_respawn_town(&clients[i]);
                send_text_to_client(
                    clients[i].sock,
                    "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
            }
            send_detailed_state(&clients[i]);
        }
    }

    if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_BLEEDING)) {
        clients[i].hp -= 1;
        send_text_to_client(clients[i].sock, "[SYSTEM] You are bleeding...");
        if (clients[i].hp <= 0) {
            server_log("DEATH", "%s bled to death.", clients[i].username);
            save_bones(&clients[i]);
            player_respawn_town(&clients[i]);
            send_text_to_client(
                clients[i].sock,
                "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
        send_detailed_state(&clients[i]);
    }
}

static void respawn_traps_and_crystals(void) {
    /* --- Trap respawn --- */
    for (int f_id = 1; f_id < MAX_FLOORS; f_id++) {
        Floor *f = &master_world->floors[f_id];
        for (int t_idx = 0; t_idx < f->trap_count; t_idx++) {
            Trap *t = &f->traps[t_idx];
            if (!t->active && t->respawn_timer > 0) {
                t->respawn_timer--;
                if (t->respawn_timer == 0) {
                    t->active = true;
                }
            }
        }
    }

    /* --- Crystal respawn --- */
    for (int f_id = 0; f_id < MAX_FLOORS; f_id++) {
        Floor *f = &master_world->floors[f_id];
        for (int i = 0; i < f->crystal_respawn_count; i++) {
            CrystalRespawn *cr = &f->crystal_respawns[i];
            if (cr->respawn_timer > 0) {
                cr->respawn_timer--;
                if (cr->respawn_timer == 0) {
                    // Restore the crystal in the map
                    f->map.data[0][cr->y][cr->x] = cr->type;

                    // Broadcast VFX to clients
                    float r = 1.0f, g = 1.0f, b = 1.0f;
                    if (cr->type == VOXEL_CRYSTAL_BLUE) {
                        r = 0.3f;
                        g = 0.7f;
                        b = 1.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_PURPLE) {
                        r = 0.8f;
                        g = 0.2f;
                        b = 1.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_RED) {
                        r = 1.0f;
                        g = 0.1f;
                        b = 0.1f;
                    } else if (cr->type == VOXEL_CRYSTAL_GREEN) {
                        r = 0.1f;
                        g = 1.0f;
                        b = 0.2f;
                    } else if (cr->type == VOXEL_CRYSTAL_YELLOW) {
                        r = 1.0f;
                        g = 0.9f;
                        b = 0.1f;
                    } else if (cr->type == VOXEL_CRYSTAL_ORANGE) {
                        r = 1.0f;
                        g = 0.5f;
                        b = 0.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_CYAN) {
                        r = 0.0f;
                        g = 0.9f;
                        b = 1.0f;
                    }

                    broadcast_spell_vfx(cr->x, cr->y, cr->x, cr->y, 1, r, g, b, f_id);

                    // Remove from list
                    f->crystal_respawns[i] = f->crystal_respawns[f->crystal_respawn_count - 1];
                    f->crystal_respawn_count--;
                    i--; // re-check this index
                }
            }
        }
    }
}

/*One tick of an NPC: on a new round its effects, saving throws and
 * periodic damage, then as many AI cycles as its energy allows; a dead
 * NPC counts down to its respawn instead.*/
static void npc_round(Client *clients, NPC *npcs, NPC *n, bool new_round) {
    /*Merchants with respawn_timer == -1: Always skipped*/
    if (n->respawn_timer < 0) {
        return;
    }

    if (n->active) {
        /* Living NPC: update effects and AI */
        if (new_round) {
            rules_update_effects(n->effects, &n->effect_count);

            /* --- Periodic effects: NPC poison --- */
            if (rules_has_condition_t(n->effects, n->effect_count, COND_POISONED)) {
                n->hp -= 1;
                if (n->hp <= 0) {
                    n->hp = 0;
                    npc_set_active(n, false);
                    n->respawn_timer = 60;
                    if (n->archetype == ARCH_BOSS) {
                        handle_boss_death(NULL, n);
                    }
                }
            }

            /* --- NPC Morale: below 20% HP rolls a saving throw vs Frightened --- */
            if (n->hp < (n->max_hp / 5) && n->morale == 0) {
                n->morale = 1;
                int roll_v = 0;
                bool success = rules_roll_save(0, 12, false, false, &roll_v);
                if (!success) {
                    if (n->effect_count < MAX_EFFECTS_PER_ENTITY) {
                        n->effects[n->effect_count].name = "Frightened";
                        n->effects[n->effect_count].duration_rounds = 5;
                        n->effect_count++;
                        // clog_save(n->template->name, "Morale", roll_v, 0, 12, false);
                    }
                }
            }

            /*--- NPC saving throws for condition recovery ---*/
            /*Compared BY VALUE (ConditionType): the display name comes from
       *the canonical table, so a renamed string can't break the logic.*/
            const ConditionType npc_conditions[] = {COND_PARALYZED, COND_STUNNED, COND_UNCONSCIOUS,
                                                    COND_PETRIFIED, COND_FROZEN};
            for (int c_idx = 0; c_idx < 5; c_idx++) {
                if (!rules_has_condition_t(n->effects, n->effect_count, npc_conditions[c_idx])) {
                    continue;
                }
                bool dis = rules_has_condition_t(n->effects, n->effect_count, COND_CURSED);
                int roll_v = 0;
                /*Base modifier +2 for NPCs*/
                bool success = rules_roll_save(2, 12, false, dis, &roll_v);
                /*npc_name(): an NPC with effects but template==NULL (a
         * summoned elemental under a cloud) must not deref the
         * template.*/
                clog_save(npc_name(n), condition_to_name(npc_conditions[c_idx]), roll_v, 2, 12,
                          success);
                if (success) {
                    const char *cond_name = condition_to_name(npc_conditions[c_idx]);
                    for (int e_idx = 0; e_idx < n->effect_count; e_idx++) {
                        if (strcasecmp(n->effects[e_idx].name, cond_name) == 0) {
                            n->effects[e_idx] = n->effects[n->effect_count - 1];
                            n->effect_count--;
                            break;
                        }
                    }
                }
            }

            /*--- Merchant store replenishment management ---*/
            if (n->archetype == ARCH_MERCHANT) {
                if (n->merchant.restock_timer > 0) {
                    n->merchant.restock_timer--;
                } else {
                    for (int j = 0; j < n->merchant.item_count; j++) {
                        n->merchant.item_stock[j] = n->merchant.item_stock_max[j];
                    }
                    n->merchant.restock_timer = 100; /*Restock every 100 rounds ≈ 10 min*/
                }
            }
        } /*end new_round for NPC*/

        /*--- Block actions for debilitating conditions ---*/
        if (rules_has_condition_t(n->effects, n->effect_count, COND_PARALYZED) ||
            rules_has_condition_t(n->effects, n->effect_count, COND_STUNNED) ||
            rules_has_condition_t(n->effects, n->effect_count, COND_PETRIFIED) ||
            rules_has_condition_t(n->effects, n->effect_count, COND_FROZEN) ||
            rules_has_condition_t(n->effects, n->effect_count, COND_UNCONSCIOUS)) {
            return;
        }

        /* --- Speed/Tick System (Phase 4) ---
     * The NPC accumulates energy every tick equal to its speed.
     * It only acts when it has enough energy (threshold = ENERGY_THRESHOLD).
     * Speed 1 = slow (Golem), 2 = normal, 3 = fast (Spider). */
        int npc_speed = (n->template && n->template->speed > 0) ? n->template->speed : 2;
        n->energy += npc_speed;

        /*Each NPC gets at most ONE action (attack/cast) per round: the
     * first energy cycle of THIS round runs with new_round, extra
     * cycles (fast NPCs with surplus energy) only move.*/
        bool first_cycle = true;
        while (n->energy >= ENERGY_THRESHOLD) {
            n->energy -= ENERGY_THRESHOLD;
            ai_update_npc(n, clients, MAX_CLIENTS, new_round && first_cycle, npcs);
            first_cycle = false;
        }

    } else if (n->template != NULL && !n->is_ghost && n->archetype != ARCH_TREASURE &&
               n->archetype != ARCH_GOLD) {
        /* Dead NPC: respawn countdown */
        if (new_round && n->respawn_timer > 0) {
            n->respawn_timer--;
        }
        if (n->respawn_timer == 0) {
            /*Bounds: a corrupted npcs.dat can carry an out-of-range floor
       * or spawn point. Leave the slot dead (the density monitor
       * skips it too) instead of indexing the world out of bounds.*/
            if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS || n->spawn_x < 0 ||
                n->spawn_x >= MAP_WIDTH || n->spawn_y < 0 || n->spawn_y >= MAP_HEIGHT) {
                return;
            }
            /* Ensure the spawn tile is free, otherwise delay */
            if (master_world->floors[n->floor_id].entity_grid[n->spawn_y][n->spawn_x] != 0) {
                n->respawn_timer = 5;
                return;
            }
            /*Revives at the spawn location (npc_set_active keeps the floor
       * caches in step).*/
            npc_set_active(n, true);
            n->x = n->spawn_x;
            n->y = n->spawn_y;
            n->hp = n->max_hp;
            n->effect_count = 0;
            n->morale = 0;
            n->respawn_timer = 0;
            master_world->floors[n->floor_id].entity_grid[n->y][n->x] = n->entity_id;
            ai_init_npc(n, n->template->name, n->floor_id);
            server_log("SPAWN", "%s respawned on floor %d (%d,%d)", n->template->name, n->floor_id,
                       n->x, n->y);
        }
    }
}

/*A floor whose living NPCs fall under DENSITY_MIN_PCT gets one of its dead
 * NPCs back at its spawn point (one per floor per check).*/
static void rebalance_floor_density(NPC *npcs) {
    /*Rebuild the cache only if reported dirty (first boot or load)*/
    if (g_floor_stats_dirty) {
        floor_stats_rebuild(npcs);
    }

    for (int f = 1; f < MAX_FLOORS; f++) {
        int total = g_floor_stats[f].total;
        int active = g_floor_stats[f].active;

        if (total == 0) {
            continue;
        }
        int pct = (active * 100) / total;
        if (pct >= DENSITY_MIN_PCT) {
            continue;
        }

        /* Density too low: force respawn of the first dead NPC on
     * this floor. O(N) scan — but performed rarely and only
     * on floors in emergency, not on all of them. */
        for (int i = 0; i < MAX_NPCS; i++) {
            NPC *n = &npcs[i];
            if (n->active) {
                continue;
            }
            if (n->template == NULL) {
                continue;
            }
            if (n->archetype == ARCH_MERCHANT) {
                continue;
            }
            if (n->floor_id != f) {
                continue;
            }
            if (n->respawn_timer < 0) {
                continue;
            }
            if (n->spawn_x < 0 || n->spawn_x >= MAP_WIDTH || n->spawn_y < 0 ||
                n->spawn_y >= MAP_HEIGHT) {
                continue; /*corrupted spawn point: never index the grid OOB*/
            }
            if (master_world->floors[f].entity_grid[n->spawn_y][n->spawn_x] != 0) {
                continue; /* Skip and try another NPC if this spawn spot is blocked */
            }
            npc_set_active(n, true); /* Updates the floor caches */
            n->x = n->spawn_x;
            n->y = n->spawn_y;
            n->hp = n->max_hp;
            n->effect_count = 0;
            n->morale = 0;
            n->respawn_timer = 0;
            master_world->floors[f].entity_grid[n->y][n->x] = n->entity_id;
            ai_init_npc(n, n->template->name, n->floor_id);
            server_log("SPAWN", "[DENSITY] Floor %d (%d%%): emergency %s", f, pct,
                       n->template->name);
            break; /*One at a time so as not to overload*/
        }
    }
}

void update_world(Client *clients, NPC *npcs) {
    static long long last_tick_ms = 0;
    long long now_ms = get_time_ms();

    autosave_if_due(clients, npcs);

    /*The main loop calls this on the fixed-step clock, so a round passes on
   * (almost) every call; this gate stays the definition of a round.*/
    if (last_tick_ms == 0) {
        last_tick_ms = now_ms;
    }
    bool new_round = (now_ms - last_tick_ms >= TICK_MS);
    if (new_round) {
        last_tick_ms = now_ms;
        global_total_turns++;
        if (global_total_turns % 5 == 0) {
            broadcast_game_time(clients);
        }
        update_global_event(clients);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            player_round(clients, i);
        }
        respawn_traps_and_crystals();
    }

    /*Every NPC sees the same round flag: each one decays its effects and
   * acts once per round (first_cycle in npc_round), and every dead one
   * counts down.*/
    for (int i = 0; i < MAX_NPCS; i++) {
        npc_round(clients, npcs, &npcs[i], new_round);
    }

    /*Spawns, deaths and splits of this tick.*/
    floor_index_rebuild(npcs);
    /*The AI moved the monsters: realign the entity grid, or players would
   * walk through them.*/
    if (new_round) {
        sync_entity_grid(npcs);
    }
    if (new_round && (global_total_turns % DENSITY_CHECK == 0)) {
        rebalance_floor_density(npcs);
    }
    if (new_round) {
        aoe_update_clouds(npcs, MAX_NPCS, clients, MAX_CLIENTS);
    }
}

/*pickup_tombstone_items has been replaced by tombstone_pickup() in tombstone.c*/
