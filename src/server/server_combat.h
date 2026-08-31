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
 * Implementation: src/server/main_server.c (to be migrated to server_combat.c)*/
#ifndef SERVER_COMBAT_H
#define SERVER_COMBAT_H

#include <stdbool.h>
#include "../../include/net.h"
#include "server_entities.h"

/**
 * perform_attack - Performs a player attack on an NPC.
 * @c: Pointer to the attacking client.
 * @t: Pointer to the target NPC.
 * @npcs: Array of all NPCs (for secondary AOEs).*/
void perform_attack(Client *c, NPC *t, NPC *npcs);

/**
 * perform_attack_npc - Performs an NPC attack on a player.
 * @n: Pointer to the attacking NPC.
 * @c: Pointer to the target client.
 * @npcs: Array of all NPCs.*/
void perform_attack_npc(NPC *n, Client *c, NPC *npcs);

/**
 * send_text_to_client - Send a text message to a client.
 * @sock: Socket of the recipient client.
 * @fmt: Printf format of the message.*/
void send_text_to_client(int sock, const char *fmt, ...);

/**
 * broadcast_spell_vfx - Broadcast VFX spell to all clients on the plan.*/
void broadcast_spell_vfx(int sx, int sy, int tx, int ty,
                          int vfx_type, float r, float g, float b,
                          int floor_id);

#endif /* SERVER_COMBAT_H */
