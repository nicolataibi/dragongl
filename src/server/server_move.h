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

/* server_move.h - One step of a player (MSG_MOVE). */
#ifndef SERVER_MOVE_H
#define SERVER_MOVE_H

#include "protocol.h"
#include "server_entities.h"

/* Rate-limited step of one tile towards (m->dx, m->dy), each clamped to
 * -1..1: meets the entity on the tile (merchant, loot, monster, player) or
 * walks there, then sends the new state and, if the view moved, the map. */
void player_move(Client *c, const MsgMove *m, Client *clients, Game *game);

#endif /* SERVER_MOVE_H */
