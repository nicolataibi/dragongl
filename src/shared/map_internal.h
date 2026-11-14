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

/*map_internal.h - What the world generator shares between its files.*/

#ifndef MAP_INTERNAL_H
#define MAP_INTERNAL_H

#include "map.h"

/*Fills the rectangle (clipped to the map) with a voxel.*/
void map_fill_rect(Map *map, int x, int y, int w, int h, VoxelType type);
/*Floor 0: the square, the class temples, the ring of shops, the stairs.*/
void generate_city(Map *map);
/*Arms a new dungeon floor: floor, wall, crystal and gas traps.*/
void spawn_traps_on_floor(Floor *f);

#endif /* MAP_INTERNAL_H */
