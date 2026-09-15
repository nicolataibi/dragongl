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
 * Time complexity: O(n log n) — the open list is a REAL binary heap
 * (the previous "implicit heap" was a sorted array: O(n) per push/pop,
 * i.e. O(n^2) overall on long searches) with decrease-key on
 * re-opening.
 * Space complexity: O(MAP_WIDTH * MAP_HEIGHT) for the node grid.
 *
 * The grid is NOT memset on every query (it is ~2.9 MB at 300x300 and
 * A* runs on every AI step of every monster): each node carries a
 * per-query STAMP, and a new query just bumps the global counter.
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
    int heap_pos;   /*index in the open heap, -1 if absent*/
    uint32_t open_stamp;   /*query id while in the open list*/
    uint32_t closed_stamp; /*query id once closed*/
} AStarNode;

/* Static grid: MAP_HEIGHT * MAP_WIDTH is at most 300*300 = 90000 nodes */
static AStarNode grid[MAP_HEIGHT][MAP_WIDTH];

/*Query counter for the stamp fields (a query id of 0 means "never
 * visited" — the static grid is zero-initialized). Wraps to 1 and
 * wipes all stamps only on the astronomical 32-bit overflow.*/
static uint32_t g_query_id = 0;

/*Open list: a min-heap of node pointers keyed by f (ties broken by the
 * larger g, so the search prefers the deeper frontier — standard A*
 * tie-break). Index of every heap element in the array: heap_pos.*/
static AStarNode *open_heap[MAX_ASTAR_OPEN];
static int        heap_count;

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

/*Node ordering: lower f first; on equal f, larger g first (deeper node).*/
static inline int heap_less(const AStarNode *a, const AStarNode *b) {
    if (a->f != b->f) return a->f < b->f;
    return a->g > b->g;
}

static void heap_sift_up(int i) {
    AStarNode *node = open_heap[i];
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!heap_less(node, open_heap[parent])) {
            break;
        }
        open_heap[i] = open_heap[parent];
        open_heap[i]->heap_pos = i;
        i = parent;
    }
    open_heap[i] = node;
    node->heap_pos = i;
}

static void heap_sift_down(int i) {
    AStarNode *node = open_heap[i];
    int n = heap_count;
    for (;;) {
        int l = 2 * i + 1;
        int r = 2 * i + 2;
        int best = i;
        if (l < n && heap_less(open_heap[l], open_heap[best])) {
            best = l;
        }
        if (r < n && heap_less(open_heap[r], open_heap[best])) {
            best = r;
        }
        if (best == i) {
            break;
        }
        open_heap[i] = open_heap[best];
        open_heap[i]->heap_pos = i;
        i = best;
    }
    open_heap[i] = node;
    node->heap_pos = i;
}

/*Pushes a node into the heap (O(log n)). Returns false when the queue
 * is full: the node's open_stamp is then LEFT UNSET, so a later
 * re-evaluation will retry the push if room has opened up.*/
static bool open_push(AStarNode *node) {
    if (heap_count >= MAX_ASTAR_OPEN) {
        node->heap_pos = -1;
        return false;
    }
    open_heap[heap_count] = node;
    node->heap_pos = heap_count;
    heap_count++;
    node->open_stamp = g_query_id;
    heap_sift_up(node->heap_pos);
    return true;
}

/*Decrease-key: f only ever decreases in A*, so a sift-up suffices.*/
static void open_decrease_key(AStarNode *node) {
    if (node->heap_pos >= 0) {
        heap_sift_up(node->heap_pos);
    }
}

/*Extracts the node with the smallest f (root of the heap, O(log n)).*/
static AStarNode *open_pop(void) {
    if (heap_count == 0) {
        return NULL;
    }
    AStarNode *best = open_heap[0];
    heap_count--;
    if (heap_count > 0) {
        open_heap[0] = open_heap[heap_count];
        open_heap[0]->heap_pos = 0;
        heap_sift_down(0);
    }
    best->heap_pos = -1;
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

    /*Stamp-based reset: O(1) instead of the old 2.9 MB memset. A node is
     * "open"/"closed" for THIS query iff its stamp equals g_query_id.*/
    g_query_id++;
    if (g_query_id == 0) { /*uint32 overflow: wipe the stamps once*/
        for (int i = 0; i < MAP_HEIGHT * MAP_WIDTH; i++) {
            grid[i / MAP_WIDTH][i % MAP_WIDTH].open_stamp = 0;
            grid[i / MAP_WIDTH][i % MAP_WIDTH].closed_stamp = 0;
        }
        g_query_id = 1;
    }
    heap_count = 0;

    /*Starting node*/
    AStarNode *start = &grid[sy][sx];
    start->x        = sx;
    start->y        = sy;
    start->g        = 0;
    start->f        = heuristic(sx, sy, tx, ty);
    start->parent_x = -1;
    start->parent_y = -1;
    start->heap_pos = -1;
    start->open_stamp = g_query_id;
    open_push(start);

    /*Directions: up, down, left, right*/
    const int dx[4] = { 0,  0, -1, 1};
    const int dy[4] = {-1,  1,  0, 0};

    while (heap_count > 0) {
        AStarNode *current = open_pop();
        current->open_stamp = 0;
        current->closed_stamp = g_query_id;

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
            /*I add the starting node. If the buffer is full the true path
             * has more nodes than it can hold: return "no path" so the
             * caller falls back to a SAFE single step. A truncated path
             * would not start at the source, and using its path[1] would
             * teleport the entity. The old code wrote reverse[path_len]
             * unconditionally here: with path_len == max_steps that was
             * a one-element STACK BUFFER OVERFLOW.*/
            if (path_len >= max_steps) {
                return 0;
            }
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
            if (neighbor->closed_stamp == g_query_id) {
                continue;
            }

            int tentative_g = current->g + 1;
            bool in_open = (neighbor->open_stamp == g_query_id);

            if (!in_open || tentative_g < neighbor->g) {
                neighbor->x        = nx;
                neighbor->y        = ny;
                neighbor->g        = tentative_g;
                neighbor->f        = tentative_g + heuristic(nx, ny, tx, ty);
                neighbor->parent_x = cx;
                neighbor->parent_y = cy;

                if (!in_open) {
                    if (!open_push(neighbor)) {
                        /*Queue full right now: it will be retried if the
                         * neighbor is re-evaluated with a better g.*/
                        continue;
                    }
                } else {
                    /*f decreased: restore the heap order (decrease-key)*/
                    open_decrease_key(neighbor);
                }
            }
        }
    }

    /*No paths found*/
    return 0;
}
