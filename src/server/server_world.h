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

#ifndef SERVER_WORLD_H
#define SERVER_WORLD_H

#include "server_entities.h"
#include "net.h"

/*Hour and minute of the game day (1 round = 1 game minute).*/
void get_game_time(Game *game, int *h, int *m);

void update_world(Client *clients, Game *game);
/*Save world.dat + npcs.dat to the state directory (false on failure).*/
bool save_world_state(Game *game);

/* Floor Stats Cache — incremental O(1) updates */
void floor_stats_rebuild(Game *game);
/*Counters of one floor (rebuilding the cache when stale); false when the
 * floor is out of range.*/
bool floor_stats_get(Game *game, int floor_id, int *total, int *active);
void floor_stats_npc_died(int floor_id);
void floor_stats_npc_spawned(int floor_id);

/* THE single point where an NPC's alive/dead state changes: sets
 * n->active and, in the same step, keeps the per-floor O(1) stats cache
 * consistent (the density monitor reads it). It applies exactly the same
 * accounting as floor_stats_rebuild() — merchants are excluded, slots
 * without a template (gold piles, chests, dropped items, summons) and
 * out-of-range floors are ignored — so the cache can never drift.
 * It also adds a newly active NPC to the per-floor index. Use it at EVERY
 * spawn/death site, never write n->active by hand, and set the final
 * fields (floor, template, archetype) first.*/
void npc_set_active(Game *game, NPC *n, bool active);

/*A slot for a new NPC: one that never held a creature or holds the
 * remains of an item, else the slot of a dead monster (which will then
 * not respawn). NULL when every slot is alive. The caller fills the
 * fields and activates it with npc_set_active().*/
NPC *npc_alloc(Game *game);

/*An item lying on the ground at (x, y): the NPC that carries it (archetype
 * ARCH_TREASURE, registered on the entity grid), or NULL without a slot.*/
NPC *npc_spawn_item(Game *game, int floor_id, int x, int y, const ItemInstance *item);

/* Per-floor index of active NPCs — local queries (broadcast on a step,
 * aggro on an attack, swarm splits) iterate O(entities on the floor)
 * instead of scanning all MAX_NPCS (50k) slots.
 * Rebuilt once per world tick (and at startup); npc_set_active() adds
 * the NPCs that become active in between, and a dead NPC keeps its entry
 * until the rebuild (consumers check it). When a floor would overflow the
 * index, floor_index_for() returns NULL and the caller scans every slot.
 * Use floor_iter() below rather than the raw index.*/
void floor_index_rebuild(Game *game);
const int *floor_index_for(int floor_id, int *count_out);

/* Walk of the active NPCs of one floor, through the per-floor index (every
 * slot when the floor overflowed it): the same slots, in the same
 * increasing order, as a scan of all MAX_NPCS slots that keeps the active
 * NPCs of that floor, including NPCs activated during the walk.
 *   FloorIter it = floor_iter(npcs, f);
 *   for (int i; (i = floor_iter_next(&it)) >= 0;) use(&npcs[i]);        */
typedef struct {
    const NPC *npcs;
    int floor_id;
    int last; /*slot returned last, -1 before the first*/
} FloorIter;
FloorIter floor_iter(Game *game, int floor_id);
int floor_iter_next(FloorIter *it);

/* The active NPC with this entity id, or NULL. O(1) through a map rebuilt
 * with the floor index; an NPC spawned since then is found by one scan. */
NPC *npc_by_entity_id(Game *game, int entity_id);

#endif // SERVER_WORLD_H
