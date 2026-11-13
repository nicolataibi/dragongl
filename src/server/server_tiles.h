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

/*
 * server_tiles.h - Tiles under the players.
 */

#ifndef SERVER_TILES_H
#define SERVER_TILES_H

#include "map.h"
#include "server_entities.h"

/*Rebuilds the entity grid of every floor from the NPCs and the players.*/
void sync_entity_grid(Game *game);
/*A tile a character can stand on (never the outermost ring of the map).*/
bool tile_walkable(const Floor *fl, int x, int y);
/*A walkable tile with no creature, player or item on it.*/
bool tile_free(const Floor *fl, int x, int y);
/*A walkable tile near (ax, ay), in rings up to radius 4; always in bounds
 * (worst case the clamped anchor).*/
void find_walkable_near(const Floor *fl, int ax, int ay, int *ox, int *oy);
/*Opens the city shop doors by day, closes them by night.*/
void update_city_doors(Game *game);
/*Remembers the deepest floor the character has stood on (class transit
 * cantrips). Call it every time a client's floor_id changes.*/
void client_track_explored_floor(Client *c);
/*What happens on the tile the player just stepped on: stairs, portals,
 * hazards, traps.*/
void check_tile_events(Client *c, Game *game);

#endif /* SERVER_TILES_H */
