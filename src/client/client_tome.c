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
 * Floating tome of The Archive of a Thousand Battles (see client_tome.h).
 *
 * Behaviour:
 *  - POSITION: every 1.2..3.5 s a new random waypoint is picked on a ring
 *    around the merchant (random angle, random radius), clamped to the
 *    shop's flight box. The book steers towards the waypoint with a
 *    first-order velocity lag, so the trajectory is a smooth, meandering,
 *    random path that never leaves the shop interior. A small sinusoidal
 *    bob is added on top.
 *  - ROTATION: on every new waypoint a new random spin is drawn (random
 *    unit axis x random speed 1.2..5.0 rad/s); the actual angular velocity
 *    relaxes towards that target, so the book tumbles on itself with
 *    smoothly changing, random rotation.
 *
 * The room bounds come from a 4-direction scan of the local map starting
 * at the merchant tile (rock/wall/door/water/lava stop the scan), so the
 * book also stays inside if the room shape changes.
 */

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "client_state.h"   /* CLIENT_MAX_ENTITIES */
#include "client_tome.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ─────────────────────────── Tuning ─────────────────────────── */
#define TOME_SCAN_MAX     10    /* max tiles scanned per direction      */
#define TOME_WALL_MARGIN  0.85f /* keep this far from the room walls    */
#define TOME_MIN_HALF     0.35f /* fallback box minimum half extent     */
#define TOME_MAX_HALF     3.5f  /* flight box maximum half extent       */
#define TOME_FALLBACK_HALF 1.0f /* box used until the map is loaded     */
#define TOME_CLEARANCE    0.35f /* corner clearance inside the box      */
#define TOME_MIN_Y        0.95f /* minimum flight height (tiles)        */
#define TOME_MAX_Y        2.05f /* maximum flight height (tiles)        */
#define TOME_SPEED        1.5f  /* cruise speed, tiles/s                */
#define TOME_BOB_AMP      0.07f /* sinusoidal bob amplitude, tiles      */
#define TOME_BOB_FREQ     2.1f  /* sinusoidal bob frequency, rad/s      */
#define TOME_MIN_SPIN     1.2f  /* min random spin speed, rad/s         */
#define TOME_MAX_SPIN     5.0f  /* max random spin speed, rad/s         */
/* ──────────────────────────────────────────────────────────────── */

typedef struct {
    bool   ready;         /* initialized for the current owner          */
    int    owner_id;      /* entity id this state belongs to            */
    int    owner_floor;   /* floor the entity was seen on               */

    /* Flight box: half extents (tiles) around the merchant tile */
    float  hx, hz;
    bool   bounds_known;  /* false = fallback box (map not loaded yet) */
    int    bounds_mx, bounds_my; /* merchant tile the box was scanned for */

    /* Book center, ABSOLUTE world tile coordinates (y = height) */
    float  px, py, pz;
    float  vx, vz;        /* horizontal velocity (tiles/s)          */

    /* Current random waypoint (absolute coords) */
    float  wx, wy, wz;
    float  wp_timer;      /* s until the next waypoint is drawn     */

    /* Angular velocity (axis x speed, rad/s) and its random target */
    float  spin[3];
    float  spin_target[3];

    /* Orientation: column-major 4x4 rotation matrix */
    float  orient[16];

    float  wobble;        /* bob phase (rad) */
    uint32_t rng;         /* xorshift32 state */
} TomeAnim;

static TomeAnim g_tome[CLIENT_MAX_ENTITIES];

/* ─────────────────────────── RNG ─────────────────────────── */

static uint32_t tome_rng(TomeAnim *a) {
    uint32_t x = a->rng ? a->rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rng = x;
    return x;
}

/* float in [0,1) */
static float tome_frand(TomeAnim *a) {
    return (float)(tome_rng(a) >> 8) * (1.0f / 16777216.0f);
}

/* ─────────────────── Matrix helpers (4x4, column-major) ─────────────────── */

static void tome_mat4_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b (column-major) */
static void tome_mat4_mul(float out[16], const float a[16], const float b[16]) {
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[row]       * b[col * 4]     +
                               a[4 + row]   * b[col * 4 + 1] +
                               a[8 + row]   * b[col * 4 + 2] +
                               a[12 + row]  * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Rotation of `theta` radians around the unit axis (Rodrigues), 4x4. */
static void tome_mat4_axis_angle(float m[16], const float axis[3], float theta) {
    float x = axis[0], y = axis[1], z = axis[2];
    float s = sinf(theta), c = cosf(theta), t = 1.0f - c;
    m[0]  = t * x * x + c;     m[4]  = t * x * y - s * z;   m[8]  = t * x * z + s * y;
    m[1]  = t * x * y + s * z; m[5]  = t * y * y + c;       m[9]  = t * y * z - s * x;
    m[2]  = t * x * z - s * y; m[6]  = t * y * z + s * x;   m[10] = t * z * z + c;
    m[3]  = 0.0f; m[7] = 0.0f; m[11] = 0.0f;
    m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;
}

/* ─────────────────────── Room bounds ─────────────────────── */

/* Blocking tiles: stop the book's flight box. The door is blocking ON
 * PURPOSE — the book must stay inside the shop, never drift out through
 * the entrance. VOXEL_ROCK doubles as "map not loaded yet". */
static bool tome_blocks(TileType t) {
    return t == VOXEL_ROCK || t == VOXEL_WALL || t == VOXEL_OBSIDIAN ||
           t == VOXEL_GOLD_VEIN || t == VOXEL_DOOR || t == VOXEL_WATER ||
           t == VOXEL_LAVA;
}

/* Number of free tiles from (mx,my) towards (dx,dy) before the first
 * blocking tile (or the scan cap / map edge). */
static int tome_scan(const TileType *map, int mx, int my, int dx, int dy) {
    for (int s = 1; s <= TOME_SCAN_MAX; s++) {
        int x = mx + dx * s;
        int y = my + dy * s;
        if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return s - 1;
        if (tome_blocks(map[y * MAP_WIDTH + x])) return s - 1;
    }
    return TOME_SCAN_MAX;
}

static void tome_recompute_bounds(TomeAnim *a, int mx, int my, const TileType *map) {
    a->bounds_mx = mx;
    a->bounds_my = my;
    a->bounds_known = false;

    if (!map) return;
    if (mx < 0 || mx >= MAP_WIDTH || my < 0 || my >= MAP_HEIGHT) return;
    /* Merchant tile still unknown (VOXEL_ROCK): the map chunk has not
     * arrived yet — keep the fallback box and retry next frame. */
    if (tome_blocks(map[my * MAP_WIDTH + mx])) return;

    int west  = tome_scan(map, mx, my, -1, 0);
    int east  = tome_scan(map, mx, my,  1, 0);
    int north = tome_scan(map, mx, my,  0, -1);
    int south = tome_scan(map, mx, my,  0,  1);
    int w = west  < east  ? west  : east;
    int n = north < south ? north : south;

    float hx = (float)w - TOME_WALL_MARGIN;
    float hz = (float)n - TOME_WALL_MARGIN;
    if (hx < TOME_MIN_HALF) hx = TOME_MIN_HALF;
    if (hx > TOME_MAX_HALF) hx = TOME_MAX_HALF;
    if (hz < TOME_MIN_HALF) hz = TOME_MIN_HALF;
    if (hz > TOME_MAX_HALF) hz = TOME_MAX_HALF;
    a->hx = hx;
    a->hz = hz;
    a->bounds_known = true;
}

/* ─────────────────── Waypoint + spin re-roll ─────────────────── */

/* New random waypoint: random angle + random radius around the merchant
 * (so the book orbits its owner), clamped to the shop flight box. A new
 * random self-rotation is drawn at the same time. */
static void tome_new_waypoint(TomeAnim *a, int mx, int my) {
    a->wp_timer = 1.2f + tome_frand(a) * 2.3f; /* 1.2 .. 3.5 s */

    float ang = tome_frand(a) * 2.0f * (float)M_PI;
    float maxr = (a->hx < a->hz ? a->hx : a->hz) - 0.4f;
    if (maxr < 0.5f) maxr = 0.5f;
    float minr = 0.7f;
    if (minr > maxr) minr = maxr * 0.8f;
    float r = minr + tome_frand(a) * (maxr - minr);

    a->wx = (float)mx + cosf(ang) * r;
    a->wz = (float)my + sinf(ang) * r;

    /* Clamp to the flight box (guards the tiny-box edge case too) */
    {
        float lo = (float)mx - a->hx + TOME_CLEARANCE;
        float hi = (float)mx + a->hx - TOME_CLEARANCE;
        if (hi < lo) { lo = hi = (float)mx; }
        if (a->wx < lo) a->wx = lo;
        if (a->wx > hi) a->wx = hi;
        lo = (float)my - a->hz + TOME_CLEARANCE;
        hi = (float)my + a->hz - TOME_CLEARANCE;
        if (hi < lo) { lo = hi = (float)my; }
        if (a->wz < lo) a->wz = lo;
        if (a->wz > hi) a->wz = hi;
    }
    a->wy = TOME_MIN_Y + tome_frand(a) * (TOME_MAX_Y - TOME_MIN_Y);

    /* New random spin: random unit axis x random speed */
    float ax, ay, az, len2;
    do {
        ax = tome_frand(a) * 2.0f - 1.0f;
        ay = tome_frand(a) * 2.0f - 1.0f;
        az = tome_frand(a) * 2.0f - 1.0f;
        len2 = ax * ax + ay * ay + az * az;
    } while (len2 < 0.05f || len2 > 1.0f);
    float inv = 1.0f / sqrtf(len2);
    float speed = TOME_MIN_SPIN + tome_frand(a) * (TOME_MAX_SPIN - TOME_MIN_SPIN);
    a->spin_target[0] = ax * inv * speed;
    a->spin_target[1] = ay * inv * speed;
    a->spin_target[2] = az * inv * speed;
}

/* ─────────────────── Initialization ─────────────────── */

static void tome_init(TomeAnim *a, int slot, int entity_id, int floor_id,
                      int mx, int my) {
    memset(a, 0, sizeof(*a));
    a->ready = true;
    a->owner_id = entity_id;
    a->owner_floor = floor_id;
    a->rng = (uint32_t)(uint64_t)entity_id * 2654435761u ^
             (uint32_t)(uint64_t)floor_id * 40503u ^
             (uint32_t)time(NULL) ^ (uint32_t)(slot + 1) * 69069u;
    if (a->rng == 0) a->rng = 0x9e3779b9u;

    /* Start hovering near the merchant */
    a->px = (float)mx + (tome_frand(a) - 0.5f) * 1.2f;
    a->pz = (float)my + (tome_frand(a) - 0.5f) * 1.2f;
    a->py = 1.2f + tome_frand(a) * 0.5f;
    a->vx = a->vz = 0.0f;
    a->wobble = tome_frand(a) * 2.0f * (float)M_PI;
    a->hx = a->hz = TOME_FALLBACK_HALF;
    a->bounds_known = false;
    a->bounds_mx = -1;
    a->bounds_my = -1;
    tome_mat4_identity(a->orient);
    a->wp_timer = 0.0f; /* draw the first waypoint immediately */
}

/* ─────────────────── Public API ─────────────────── */

void tome_anim_reset_all(void) {
    memset(g_tome, 0, sizeof(g_tome));
}

void tome_anim_reset_slot(int slot) {
    if (slot < 0 || slot >= CLIENT_MAX_ENTITIES) return;
    g_tome[slot].ready = false;
}

void tome_anim_update(int slot, int entity_id, int floor_id,
                      int mx, int my,
                      const TileType *map,
                      float dt,
                      float out_pos[3], float out_orient[16]) {
    /* Safe defaults: hover above the merchant, identity rotation */
    out_pos[0] = (float)mx;
    out_pos[1] = 1.25f;
    out_pos[2] = (float)my;
    tome_mat4_identity(out_orient);

    if (slot < 0 || slot >= CLIENT_MAX_ENTITIES) return;
    TomeAnim *a = &g_tome[slot];

    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    if (!a->ready || a->owner_id != entity_id || a->owner_floor != floor_id)
        tome_init(a, slot, entity_id, floor_id, mx, my);

    /* (Re-)measure the shop room until the map is known, or if the
     * merchant changed tile */
    if (!a->bounds_known || a->bounds_mx != mx || a->bounds_my != my)
        tome_recompute_bounds(a, mx, my, map);

    /* New random waypoint (and new random spin) on schedule */
    a->wp_timer -= dt;
    if (a->wp_timer <= 0.0f)
        tome_new_waypoint(a, mx, my);

    /* Horizontal steering: desired velocity toward the waypoint,
     * first-order lag on the actual velocity -> smooth random path */
    float dxw = a->wx - a->px;
    float dzw = a->wz - a->pz;
    float dist = sqrtf(dxw * dxw + dzw * dzw);
    if (dist < 0.001f) {
        a->vx = 0.0f;
        a->vz = 0.0f;
    } else {
        float s = dist < TOME_SPEED * 0.5f ? dist * 2.0f : TOME_SPEED;
        float k = 1.0f - expf(-3.0f * dt);
        a->vx += ((dxw / dist) * s - a->vx) * k;
        a->vz += ((dzw / dist) * s - a->vz) * k;
    }
    a->px += a->vx * dt;
    a->pz += a->vz * dt;

    /* Height: first-order lag toward the waypoint height */
    {
        float k = 1.0f - expf(-1.6f * dt);
        a->py += (a->wy - a->py) * k;
    }

    /* Safety clamp inside the flight box (steering already keeps the book
     * well inside; this only guards against pathological dt values) */
    {
        float lo = (float)mx - a->hx + TOME_CLEARANCE;
        float hi = (float)mx + a->hx - TOME_CLEARANCE;
        if (hi < lo) { lo = hi = (float)mx; }
        if (a->px < lo) a->px = lo;
        if (a->px > hi) a->px = hi;
        lo = (float)my - a->hz + TOME_CLEARANCE;
        hi = (float)my + a->hz - TOME_CLEARANCE;
        if (hi < lo) { lo = hi = (float)my; }
        if (a->pz < lo) a->pz = lo;
        if (a->pz > hi) a->pz = hi;
    }
    if (a->py < TOME_MIN_Y) a->py = TOME_MIN_Y;
    if (a->py > TOME_MAX_Y) a->py = TOME_MAX_Y;

    /* Bob phase */
    a->wobble += dt * TOME_BOB_FREQ;

    /* Rotation: relax the angular velocity toward the random target, then
     * integrate the orientation matrix */
    {
        float k = 1.0f - expf(-0.9f * dt);
        a->spin[0] += (a->spin_target[0] - a->spin[0]) * k;
        a->spin[1] += (a->spin_target[1] - a->spin[1]) * k;
        a->spin[2] += (a->spin_target[2] - a->spin[2]) * k;
        float om = sqrtf(a->spin[0] * a->spin[0] +
                         a->spin[1] * a->spin[1] +
                         a->spin[2] * a->spin[2]);
        if (om > 1e-4f) {
            float axis[3] = { a->spin[0] / om, a->spin[1] / om, a->spin[2] / om };
            float step[16], next[16];
            tome_mat4_axis_angle(step, axis, om * dt);
            tome_mat4_mul(next, step, a->orient);
            memcpy(a->orient, next, sizeof(next));
        }
    }

    out_pos[0] = a->px;
    out_pos[1] = a->py + TOME_BOB_AMP * sinf(a->wobble);
    out_pos[2] = a->pz;
    memcpy(out_orient, a->orient, sizeof(float) * 16);
}
