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
 * pathfinding.c — A* implementation for Dragon GL Server
 *
 * Classic A* algorithm with Manhattan heuristic.
 * Optimized for 2D tiled dungeon maps with 4-way movement
 * (N/S/E/W), as the dungeon does not use diagonal movement.
 *
 * Time complexity: O(n log n) with binary heap implicit in the open array.
 * Space complexity: O(MAP_WIDTH * MAP_HEIGHT) for the node grid.
 *
 * To maintain stack-safe allocation, the grid is statically allocated
 * with fixed dimensions equal to the maximum map.*/
#include "pathfinding.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ───────────────────────────── Strutture interne ───────────────────────── */

typedef struct {
    int x, y;
    int g;          /*cost from the starting node*/
    int f;          /* g + h (heuristic) */
    int parent_x;
    int parent_y;
    bool in_open;
    bool in_closed;
} AStarNode;

/* Static grid: MAP_HEIGHT * MAP_WIDTH is at most 128*128 = 16384 nodes */
static AStarNode grid[MAP_HEIGHT][MAP_WIDTH];

/*Open queue with sorting by f (implicit min-heap via linear array)*/
static AStarNode *open_list[MAX_ASTAR_OPEN];
static int        open_count;

/* ─────────────────────────── Internal functions ──────────────────────────── */

static inline int heuristic(int ax, int ay, int bx, int by) {
    /*Manhattan distance — best for grids without diagonals*/
    return abs(ax - bx) + abs(ay - by);
}

static inline bool tile_is_walkable(const Map *map, int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) {
        return false;
    }
    VoxelType v = map->data[0][y][x];
    /*Walkable if it is open floor*/
    return (v == VOXEL_FLOOR  ||
            v == VOXEL_DOOR   ||
            v == VOXEL_LAVA   ||
            v == VOXEL_WATER  ||
            v == VOXEL_SAND   ||
            v == VOXEL_GRASS  ||
            v == VOXEL_EMPTY  ||
            v == VOXEL_OBSIDIAN ||
            v == VOXEL_MUSHROOM_GLOW ||
            v == VOXEL_CRYSTAL_BLUE  ||
            v == VOXEL_CRYSTAL_PURPLE);
}

/*Adds a node to the open list, maintaining sorting by ascending f*/
static void open_push(AStarNode *node) {
    if (open_count >= MAX_ASTAR_OPEN) {
        return;
    }
    /* Sorted insertion (insertion sort — small list) */
    int i = open_count;
    while (i > 0 && open_list[i-1]->f > node->f) {
        open_list[i] = open_list[i-1];
        i--;
    }
    open_list[i] = node;
    open_count++;
    node->in_open = true;
}

/*Extracts the node with the smallest f (head of the sorted array)*/
static AStarNode *open_pop(void) {
    if (open_count == 0) {
        return NULL;
    }
    AStarNode *best = open_list[0];
    open_count--;
    for (int i = 0; i < open_count; i++) {
        open_list[i] = open_list[i+1];
    }
    best->in_open = false;
    return best;
}

/* ─────────────────────────── Public API ──────────────────────────────── */

int pathfind_astar(const Map *map,
                   int sx, int sy,
                   int tx, int ty,
                   PathNode *out_path,
                   int max_steps)
{
    /* Input validation */
    if (!map || !out_path || max_steps <= 0) {
        return 0;
    }
    if (sx < 0 || sx >= MAP_WIDTH || sy < 0 || sy >= MAP_HEIGHT) {
        return 0;
    }
    if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
        return 0;
    }
    if (sx == tx && sy == ty) {
        return 0;
    }

    /* Grid initialization */
    memset(grid, 0, sizeof(grid));
    open_count = 0;

    /*Starting node*/
    AStarNode *start = &grid[sy][sx];
    start->x        = sx;
    start->y        = sy;
    start->g        = 0;
    start->f        = heuristic(sx, sy, tx, ty);
    start->parent_x = -1;
    start->parent_y = -1;
    open_push(start);

    /*Directions: up, down, left, right*/
    const int dx[4] = { 0,  0, -1, 1};
    const int dy[4] = {-1,  1,  0, 0};

    while (open_count > 0) {
        AStarNode *current = open_pop();
        current->in_closed = true;

        int cx = current->x;
        int cy = current->y;

        /*Found? Let's reconstruct the path*/
        if (cx == tx && cy == ty) {
            /*Backward reconstruction*/
            int path_len = 0;
            PathNode reverse[MAX_ASTAR_PATH];
            AStarNode *node = current;

            while (node->parent_x >= 0 && path_len < max_steps) {
                reverse[path_len].x = node->x;
                reverse[path_len].y = node->y;
                path_len++;
                node = &grid[node->parent_y][node->parent_x];
            }
            /*I add the starting node*/
            reverse[path_len].x = sx;
            reverse[path_len].y = sy;
            path_len++;

            /*I reverse in out_path (start → end)*/
            int total = path_len < max_steps ? path_len : max_steps;
            for (int i = 0; i < total; i++) {
                out_path[i] = reverse[total - 1 - i];
            }
            return total;
        }

        /*We expand the neighbors (4 directions)*/
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d];
            int ny = cy + dy[d];

            if (!tile_is_walkable(map, nx, ny)) {
                continue;
            }

            AStarNode *neighbor = &grid[ny][nx];
            if (neighbor->in_closed) {
                continue;
            }

            int tentative_g = current->g + 1;

            if (!neighbor->in_open || tentative_g < neighbor->g) {
                neighbor->x        = nx;
                neighbor->y        = ny;
                neighbor->g        = tentative_g;
                neighbor->f        = tentative_g + heuristic(nx, ny, tx, ty);
                neighbor->parent_x = cx;
                neighbor->parent_y = cy;

                if (!neighbor->in_open) {
                    open_push(neighbor);
                }
            }
        }
    }

    /*No paths found*/
    return 0;
}
