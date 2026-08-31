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

#ifndef AI_H
#define AI_H

#include "server_entities.h"

// Function pointer for Behavior Tree nodes
typedef AINodeStatus (*AINodeFunc)(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs);

// Initializes the AI of an NPC
void ai_init_npc(NPC* npc, const char* monster_name, int floor_id);

//Reattaches only the behavior tree after loading from disk (no stat reset)
void ai_attach_behavior(NPC* npc);

// Main update function called by the server
void ai_update_npc(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs);

//Boss-specific behaviors
AINodeStatus ai_void_crawler_behavior(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs);
AINodeStatus ai_mage_behavior(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs);

#endif
