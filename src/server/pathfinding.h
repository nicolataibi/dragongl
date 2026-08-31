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
 * pathfinding.h — Modulo A* per Dragon GL Server
 *
 * Calcola il percorso ottimale su mappa 2D a tiles tra due coordinate,
 * rispettando i muri (VOXEL_WALL, VOXEL_SOLID, VOXEL_ROCK ecc.).
 *
 * Uso:
 *   PathNode path[MAX_ASTAR_PATH];
 *   int steps = pathfind_astar(map, sx, sy, tx, ty, path, MAX_ASTAR_PATH);
 *   if (steps > 0) {
 *       npc->x = path[1].x; //path[0] is the current position
 *       npc->y = path[1].y;
 *   }
 */
#ifndef PATHFINDING_H
#define PATHFINDING_H

#include "../../include/map.h"

#define MAX_ASTAR_PATH 128
#define MAX_ASTAR_OPEN 1024

typedef struct {
    int x, y;
} PathNode;

/**
 * pathfind_astar - Calculate the optimal path with A*.
 *
 * @map: The map of the current floor.
 * @sx, sy: Source coordinates (monster position).
 * @tx, ty: Target coordinates (target location).
 * @out_path: Output buffer where the path is written.
 * @max_steps: Maximum path length (use MAX_ASTAR_PATH).
 *
 * @return Number of nodes in the path (including start and end),
 * or 0 if no path found.
 *
 * NOTE: out_path[0] is the starting position (sx,sy).
 * out_path[1] is the first step to take.*/
int pathfind_astar(const Map *map,
                   int sx, int sy,
                   int tx, int ty,
                   PathNode *out_path,
                   int max_steps);

#endif /* PATHFINDING_H */
