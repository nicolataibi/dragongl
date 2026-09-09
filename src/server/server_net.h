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
#ifndef SERVER_NET_H
#define SERVER_NET_H

#include "server_entities.h"
#include "map.h"
#include "server_world.h"
#include <stdbool.h>

void send_text_to_client(int sock, const char *fmt, ...);
bool net_send_client(int sock, const void *data, int len);
void send_map_chunk(int sock, Map *map, int cx, int cy, int size);
void send_detailed_state(Client *c);
void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id);
void broadcast_nearby_entities(Client *c, NPC *npcs);
void notify_player_left_floor(Client *c, int old_floor);
void broadcast_player_state(Client *c);

#endif
void client_seen_reset(int ci, int floor);
