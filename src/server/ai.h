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

/*A monster attacks at +2, +1 every 4 floors down to this floor: +17 at
 * most (LORE.md, Monster Defences and Attacks).*/
#define MONSTER_ATTACK_FLOOR_CAP 60

// Function pointer for Behavior Tree nodes

// Initializes the AI of an NPC
void ai_init_npc(NPC *npc, const char *monster_name, int floor_id);

//Reattaches only the behavior tree after loading from disk (no stat reset)
void ai_attach_behavior(NPC *npc);

/*The NPC fights for the character c for `rounds` rounds: it attacks the
 * hostile creatures near it, follows c, and never attacks a character.
 * When the rounds run out, or c leaves its floor or the game, a dominated
 * creature turns hostile again and any other ally is gone.*/
void ai_make_ally(NPC *npc, const Client *c, int rounds);

/*The round of an ally, before it acts: one round less, and the end of it
 * when the rounds are over or its character is gone. False if it ended
 * (an ally is no longer: it left, or turned hostile).*/
bool ai_ally_round(Game *game, NPC *npc, Client *clients, int num_clients);

/*True for an NPC that fights for a character.*/
bool npc_is_ally(const NPC *npc);

// Main update function called by the server
void ai_update_npc(Game *game, NPC *npc, Client *clients, int num_clients, bool new_round);

//Boss-specific behaviors
AINodeStatus ai_void_crawler_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                                      bool new_round);
AINodeStatus ai_mage_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                              bool new_round);

#endif
