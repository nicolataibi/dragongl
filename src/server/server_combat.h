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
 * server_combat.h — Public interface of the combat module
 *
 * Contains declarations of used combat functions
 * across the other modules (ai.c, aoe.c, main_server.c).
 *
 * Implementation: src/server/server_combat.c*/
#ifndef SERVER_COMBAT_H
#define SERVER_COMBAT_H

#include <stdbool.h>
#include "net.h"
#include "server_entities.h"

/**
 * perform_attack - Performs a player attack on an NPC.
 * @c: Pointer to the attacking client.
 * @t: Pointer to the target NPC.
 * @npcs: Array of all NPCs (for secondary AOEs).*/
void perform_attack(Client *c, NPC *t, NPC *npcs);

/*False (after telling the player) when a condition takes his actions:
 * paralyzed, stunned, unconscious, petrified, frozen, incapacitated.*/
bool client_can_act(Client *c);

/*An NPC dies: it leaves the map and the floor caches, its killer c (and
 * c's party on the floor) gets the XP, the victory line ("falls to your
 * <by>", or the melee line when by is NULL), the event progress and the
 * loot; a boss ends its fight. c may be NULL (no one to credit). Returns
 * the XP each member received.*/
int npc_killed(Client *c, NPC *t, const char *by);

/**
 * perform_attack_npc - Performs an NPC attack on a player.
 * @n: Pointer to the attacking NPC.
 * @c: Pointer to the target client.
 * @npcs: Array of all NPCs.*/
void perform_attack_npc(NPC *n, Client *c, NPC *npcs);

#endif /* SERVER_COMBAT_H */
