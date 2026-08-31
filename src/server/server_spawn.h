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
 * server_spawn.h — Public interface of the entity spawning module
 *
 * Handles the generation of NPCs, bosses, shops, ghosts and the
 * population of the dungeons. The implementations reside in server_spawn.c.
 */
#ifndef SERVER_SPAWN_H
#define SERVER_SPAWN_H

#include "server_entities.h"
#include "items.h"
#include <stdint.h>

/**
 * populate_dungeons - Populate all floors of the dungeon with NPCs and creatures.
 * @npcs: Array of NPCs.
 * @next_id: Pointer to the unique ID counter.*/
void populate_dungeons(NPC *npcs, int *next_id);

/**
 * spawn_city_merchants - Generates city merchants (floor 0).
 * @npcs: Array of NPCs.
 * @next_id: Pointer to the unique ID counter.*/
void spawn_city_merchants(NPC *npcs, int *next_id);

/**
 * spawn_magic_shops - Spawns magic shops on the deep planes.
 * @npcs: Array of NPCs.
 * @next_id: Pointer to the unique ID counter.*/
void spawn_magic_shops(NPC *npcs, int *next_id);

/**
 * add_item_to_shop - Adds an item to a merchant's inventory.
 * @n: Recipient merchant NPC.
 * @template_idx: Index into the item_database of the template to add.
 * @stock: Initial stock quantity.
 *
 * Exposed to allow the 'dm_shop' DM command to add items
 * dynamically to a merchant's inventory during the game.*/
void add_item_to_shop(NPC *n, int template_idx, int stock);

#endif /* SERVER_SPAWN_H */
