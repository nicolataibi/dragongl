/*
 * DRAGON GL - Unit tests: A* pathfinding (pathfinding.c)
 *
 * Fixed hand-built maps (10x10 room in the corner of the 300x300
 * lattice): straight paths, forced detours, unreachable targets.
 * Run via `ctest` (see CMakeLists.txt).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pathfinding.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/*Fills the whole map with ROCK, then carves a FLOOR room at (0,0).*/
static void make_room(Map *map, int w, int h) {
    memset(map, 0, sizeof(*map));
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            map->data[0][y][x] = VOXEL_ROCK;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            map->data[0][y][x] = VOXEL_FLOOR;
        }
    }
}

static int node_at(const PathNode *path, int count, int x, int y) {
    for (int i = 0; i < count; i++) {
        if (path[i].x == x && path[i].y == y) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    Map map;

    /*--- open room: straight (Manhattan) path ---*/
    make_room(&map, 10, 10);
    PathNode path[MAX_ASTAR_PATH];
    int steps = pathfind_astar(&map, 1, 1, 8, 8, path, MAX_ASTAR_PATH);
    CHECK(steps == 15); /*7+7 steps, 15 nodes (start..end)*/
    if (steps > 0) {
        CHECK(path[0].x == 1 && path[0].y == 1);
        CHECK(path[steps - 1].x == 8 && path[steps - 1].y == 8);
        /*every consecutive pair differs by exactly 1 tile (4-way)*/
        for (int i = 1; i < steps; i++) {
            int dx = path[i].x - path[i - 1].x;
            int dy = path[i].y - path[i - 1].y;
            CHECK(abs(dx) + abs(dy) == 1);
        }
    }

    /*--- vertical wall with a gap: the detour MUST go through the gap ---*/
    make_room(&map, 10, 10);
    for (int y = 0; y < 8; y++) {
        map.data[0][y][5] = VOXEL_ROCK; /*gap at y=8,9*/
    }
    steps = pathfind_astar(&map, 1, 1, 8, 1, path, MAX_ASTAR_PATH);
    CHECK(steps > 0); /*still reachable via the gap*/
    if (steps > 0) {
        CHECK(node_at(path, steps, 5, 8) || node_at(path, steps, 5, 9));
        /*no node may stand on the wall*/
        for (int i = 0; i < steps; i++) {
            if (path[i].x == 5) {
                CHECK(path[i].y >= 8);
            }
        }
    }

    /*--- fully walled target: no path ---*/
    make_room(&map, 10, 10);
    for (int y = 0; y < 10; y++) {
        map.data[0][y][5] = VOXEL_ROCK;
    }
    steps = pathfind_astar(&map, 1, 1, 8, 1, path, MAX_ASTAR_PATH);
    CHECK(steps == 0);

    /*--- trivial / invalid inputs ---*/
    make_room(&map, 10, 10);
    CHECK(pathfind_astar(&map, 3, 3, 3, 3, path, MAX_ASTAR_PATH) == 0);
    CHECK(pathfind_astar(&map, -1, 3, 3, 3, path, MAX_ASTAR_PATH) == 0);
    CHECK(pathfind_astar(&map, 3, 3, MAP_WIDTH, 3, path, MAX_ASTAR_PATH) == 0);
    CHECK(pathfind_astar(NULL, 3, 3, 3, 3, path, MAX_ASTAR_PATH) == 0);
    CHECK(pathfind_astar(&map, 3, 3, 3, 3, NULL, MAX_ASTAR_PATH) == 0);

    /*--- determinism: two identical queries give identical paths ---*/
    make_room(&map, 10, 10);
    PathNode a[MAX_ASTAR_PATH], b[MAX_ASTAR_PATH];
    int sa = pathfind_astar(&map, 0, 0, 9, 9, a, MAX_ASTAR_PATH);
    int sb = pathfind_astar(&map, 0, 0, 9, 9, b, MAX_ASTAR_PATH);
    CHECK(sa == sb && sa > 0);
    CHECK(memcmp(a, b, sizeof(PathNode) * sa) == 0);

    /*--- repeated queries stay fast & correct (visited-stamp reset) ---*/
    for (int i = 0; i < 1000; i++) {
        int sx = i % 10, sy = (i / 10) % 10;
        int r = pathfind_astar(&map, sx, sy, 9, 9, path, MAX_ASTAR_PATH);
        if (sx == 9 && sy == 9) {
            CHECK(r == 0); /*start == target: no path by contract*/
        } else {
            CHECK(r > 0);
        }
    }

    if (g_failures == 0) {
        printf("test_pathfinding: all passed\n");
        return 0;
    }
    printf("test_pathfinding: %d FAILURES\n", g_failures);
    return 1;
}
