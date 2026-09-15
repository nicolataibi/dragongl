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
#include "../../include/net.h"

void update_world(Client *clients, NPC *npcs);

/* Floor Stats Cache — incremental O(1) updates */
void floor_stats_rebuild(NPC *npcs);
void floor_stats_npc_died(int floor_id);
void floor_stats_npc_spawned(int floor_id);

/* THE single point where an NPC's alive/dead state changes: sets
 * n->active and, in the same step, keeps the per-floor O(1) stats cache
 * consistent (the density monitor reads it). It applies exactly the same
 * accounting as floor_stats_rebuild() — merchants are excluded, slots
 * without a template (gold piles, chests, dropped items, summons) and
 * out-of-range floors are ignored — so the cache can never drift.
 * Use it at EVERY spawn/death site instead of writing n->active by hand:
 * the old code had 20+ hand-written mutations of which only a handful
 * called floor_stats_npc_died/spawned, so AoE kills, DM spawns, drops,
 * summons and ghosts all left the cache stale (floors looked fuller than
 * they were, and emergency respawns fired late).*/
void npc_set_active(NPC *n, bool active);

/* Per-floor index of active NPCs — local queries (broadcast on a step,
 * aggro on an attack, swarm splits) iterate O(entities on the floor)
 * instead of scanning all MAX_NPCS (50k) slots.
 * Rebuilt once per world tick (and at startup); entries that go stale
 * between ticks (fresh spawn, fresh death) are validated by the
 * consumer, so staleness costs a few extra checks at most. When a
 * floor would overflow the index, floor_index_for() returns NULL and
 * the caller falls back to the legacy full scan.*/
void floor_index_rebuild(const NPC *npcs);
const int *floor_index_for(int floor_id, int *count_out);

#endif // SERVER_WORLD_H
