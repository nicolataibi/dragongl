/*
 * DRAGON GL - Unit tests: floating tome animation (client_tome.c)
 *
 * Verifies the random flight of the Archive of a Thousand Battles tome:
 *  - the book stays INSIDE the shop room (never crosses the walls/door),
 *  - the trajectory actually wanders (not a fixed point),
 *  - the orientation is a proper rotation at every step and it really
 *    rotates over time (random self-rotation),
 *  - the "map not loaded yet" fallback and slot reset behave sanely.
 *
 * Pure client-logic test (client_tome.c only — no renderer, no server).
 * Run via `ctest` (see CMakeLists.txt).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client_state.h"
#include "client_tome.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* Shop identical to the city stores in map.c: 9x9 rock shell, 7x7 wood
 * floor, 3x2 ash chest, one door in the shell facing the city. */
static void build_shop_map(TileType *map, int cx, int cy) {
    memset(map, VOXEL_ROCK, sizeof(TileType) * MAP_HEIGHT * MAP_WIDTH);
    for (int y = cy - 3; y <= cy + 3; y++) {
        for (int x = cx - 3; x <= cx + 3; x++) {
            map[y * MAP_WIDTH + x] = VOXEL_WOOD;
        }
    }
    for (int y = cy - 1; y <= cy; y++) {
        for (int x = cx - 1; x <= cx + 1; x++) {
            map[y * MAP_WIDTH + x] = VOXEL_ASH;
        }
    }
    map[cy * MAP_WIDTH + (cx + 4)] = VOXEL_DOOR;
}

static void mat4_check_orthonormal(const float m[16]) {
    for (int col = 0; col < 3; col++) {
        float len2 = 0.0f;
        for (int row = 0; row < 3; row++) {
            len2 += m[col * 4 + row] * m[col * 4 + row];
        }
        CHECK(fabsf(len2 - 1.0f) < 1e-3f);
        if (col > 0) {
            float dot = 0.0f;
            for (int row = 0; row < 3; row++) {
                dot += m[col * 4 + row] * m[(col - 1) * 4 + row];
            }
            CHECK(fabsf(dot) < 1e-3f);
        }
    }
}

/* Angle (rad) between two 3D rotation matrices, via the trace. */
static float mat4_rot_angle(const float a[16], const float b[16]) {
    float tr = 0.0f;
    for (int i = 0; i < 3; i++) {
        tr += a[i * 4 + i] * b[i * 4 + i];
    }
    if (tr > 3.0f) {
        tr = 3.0f;
    }
    if (tr < 1.0f) {
        tr = 1.0f;
    }
    return acosf((tr - 1.0f) / 2.0f);
}

int main(void) {
    TileType map[MAP_HEIGHT * MAP_WIDTH];
    const int cx = 150, cy = 150; /* merchant tile = shop center */
    const float dt = 1.0f / 60.0f;

    /* === 1. The book stays inside the shop and wanders === */
    build_shop_map(map, cx, cy);
    tome_anim_reset_all();

    float pos0[3], pos[3], orient[16], orient0[16];
    tome_anim_update(0, 4242, 0, cx, cy, map, dt, pos0, orient0);
    mat4_check_orthonormal(orient0);

    float max_dx = 0.0f, max_dz = 0.0f, max_h = 0.0f, min_h = 10.0f;
    float rot_total = 0.0f;
    float prev_orient[16];
    memcpy(prev_orient, orient0, sizeof(prev_orient));
    long distinct_cells = 0;
    int last_cell = 0x7fffffff;
    for (int step = 1; step < 60 * 40; step++) { /* 40 simulated seconds */
        tome_anim_update(0, 4242, 0, cx, cy, map, dt, pos, orient);
        mat4_check_orthonormal(orient);
        rot_total += mat4_rot_angle(prev_orient, orient);
        memcpy(prev_orient, orient, sizeof(prev_orient));

        /* The shop interior spans ±3 tiles around (cx,cy); the flight box
         * keeps the book at least ~0.85 tiles from the walls. */
        float dx = pos[0] - (float)cx;
        float dz = pos[2] - (float)cy;
        CHECK(fabsf(dx) < 2.6f);
        CHECK(fabsf(dz) < 2.6f);
        CHECK(pos[1] > 0.8f && pos[1] < 2.2f);

        if (fabsf(dx) > max_dx) {
            max_dx = fabsf(dx);
        }
        if (fabsf(dz) > max_dz) {
            max_dz = fabsf(dz);
        }
        if (pos[1] > max_h) {
            max_h = pos[1];
        }
        if (pos[1] < min_h) {
            min_h = pos[1];
        }

        int cell = (int)floorf(pos[0] * 2.0f) * 100000 + (int)floorf(pos[2] * 2.0f);
        if (cell != last_cell) {
            distinct_cells++;
            last_cell = cell;
        }
    }

    /* The trajectory must actually move around (not hover in place) */
    CHECK(max_dx + max_dz > 1.0f);
    CHECK(distinct_cells > 50);
    /* ...and it must change height sometimes */
    CHECK(max_h - min_h > 0.2f);
    /* ...and it must rotate on itself over the run (accumulated angle:
     * at >= 1.2 rad/s for ~40 s the total is hundreds of radians) */
    CHECK(rot_total > 10.0f);

    /* === 2. Unknown map (all rock): fallback box, no crash === */
    tome_anim_reset_all();
    memset(map, VOXEL_ROCK, sizeof(map));
    for (int step = 0; step < 60 * 5; step++) {
        tome_anim_update(0, 4242, 0, cx, cy, map, dt, pos, orient);
        mat4_check_orthonormal(orient);
        CHECK(fabsf(pos[0] - (float)cx) < 1.2f);
        CHECK(fabsf(pos[2] - (float)cy) < 1.2f);
        CHECK(pos[1] > 0.8f && pos[1] < 2.2f);
    }

    /* === 3. Once the map chunk arrives, bounds expand (still inside) === */
    build_shop_map(map, cx, cy);
    for (int step = 0; step < 60 * 5; step++) {
        tome_anim_update(0, 4242, 0, cx, cy, map, dt, pos, orient);
        mat4_check_orthonormal(orient);
        CHECK(fabsf(pos[0] - (float)cx) < 2.6f);
        CHECK(fabsf(pos[2] - (float)cy) < 2.6f);
    }

    /* === 4. Slot reset + owner change === */
    tome_anim_reset_slot(0);
    tome_anim_update(0, 4242, 0, cx, cy, map, dt, pos, orient); /* re-init, same owner */
    tome_anim_update(0, 9999, 0, cx, cy, map, dt, pos, orient); /* different owner -> re-init */
    tome_anim_update(0, 9999, 1, cx, cy, map, dt, pos, orient); /* different floor -> re-init */
    CHECK(1); /* reaching here without a failed CHECK is the pass condition */

    if (g_failures == 0) {
        printf("OK: all tome tests passed\n");
        return 0;
    }
    printf("%d tome check(s) FAILED\n", g_failures);
    return 1;
}
