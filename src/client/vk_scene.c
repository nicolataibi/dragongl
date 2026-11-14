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
 * vk_scene.c - Instances of the 3D scene (vk_scene.h).
 */

#include "vk_scene.h"

#include "client_particles.h"
#include "client_tome.h"
#include "protocol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*Map instances are built only for a WINDOW around the player, not the
 * whole 300x300 floor: the fragment shader discards every fragment
 * further than visionRadius (max 50 in the city), and the compute cull
 * drops whatever lies beyond it.
 *
 * Rebuild policy:
 *  - skip the rebuild when the built window still covers the player's
 *    required window and no tile data changed (g_map_dirty is set by
 *    the net thread ONLY when a chunk actually differs from
 *    g_local_map, so no-op chunks cost nothing);
 *  - otherwise rebuild the window around the current position with a
 *    BUILD_MARGIN, so a rebuild re-happens only after ~MARGIN tiles of
 *    travel (amortized), not on every step.
 * Each frame slot keeps its copy of the instance list and refreshes it
 * only when its map generation is stale.*/
#define VK_MAP_VISION_MARGIN 8  /*fog fade-out slack beyond visionRadius */
#define VK_MAP_BUILD_MARGIN  16 /*rebuild amortization: tiles of travel  */
#define VK_MAP_MIN_RADIUS    16
#define VK_MAP_MAX_RADIUS    56 /*city visionRadius is 50 (50+8 -> 56)   */

typedef struct {
    int x0, y0, x1, y1;
} Window;

static bool s_map_built = false;
static int s_map_floor = -1;
static Window s_map_win = {0, 0, -1, -1};
static GpdInstance *s_map_gpd_instances = NULL;
static uint32_t s_map_gpd_capacity = 0;
static uint32_t s_map_gpd_count = 0;

typedef struct {
    float cur_x, cur_z;
    bool initialized;
} LerpPos;

static LerpPos g_entity_lerp[CLIENT_MAX_ENTITIES];

/*Entities glide to their tile instead of jumping.*/
static void lerp_update(LerpPos *lp, float tgt_x, float tgt_z, float dt) {
    const float speed = 5.0f;
    if (!lp->initialized) {
        lp->cur_x = tgt_x;
        lp->cur_z = tgt_z;
        lp->initialized = true;
        return;
    }
    lp->cur_x += (tgt_x - lp->cur_x) * dt * speed;
    lp->cur_z += (tgt_z - lp->cur_z) * dt * speed;
}

bool vk_scene_entity_pos(int slot, float *x, float *z) {
    if (slot < 0 || slot >= CLIENT_MAX_ENTITIES || !g_entity_lerp[slot].initialized) {
        return false;
    }
    *x = g_entity_lerp[slot].cur_x;
    *z = g_entity_lerp[slot].cur_z;
    return true;
}

void vk_scene_reset(void) {
    free(s_map_gpd_instances);
    s_map_gpd_instances = NULL;
    s_map_gpd_capacity = 0;
    s_map_gpd_count = 0;
    s_map_built = false;
    s_map_floor = -1;
    s_map_win.x1 = -1;
}

static bool ensure_map_gpd_capacity(uint32_t needed) {
    if (needed <= s_map_gpd_capacity) {
        return true;
    }
    uint32_t cap = s_map_gpd_capacity ? s_map_gpd_capacity : 65536u;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2u) {
            return false;
        }
        cap *= 2u;
    }
    GpdInstance *nv = realloc(s_map_gpd_instances, (size_t)cap * sizeof(*nv));
    if (!nv) {
        return false;
    }
    s_map_gpd_instances = nv;
    s_map_gpd_capacity = cap;
    return true;
}

/* --- instances ------------------------------------------------------------- */

typedef struct {
    float y;          /*height of the centre*/
    float hx, hy, hz; /*half sizes*/
    float r, g, b;
} Shape;

/*Instance kinds of the compute shaders (gpd_mesh.glsl).*/
enum { GPD_BOX = 0, GPD_PYRAMID = 1, GPD_PARTICLE = 2 };

static void push_instance(GpdInstance *instances, uint32_t *count, uint32_t max_count, int kind,
                          float cx, float cz, const Shape *sh) {
    if (!instances || *count >= max_count) {
        return;
    }
    GpdInstance *in = &instances[*count];
    in->pos[0] = cx;
    in->pos[1] = sh->y;
    in->pos[2] = cz;
    in->type = (float)kind;
    in->scale[0] = sh->hx;
    in->scale[1] = sh->hy;
    in->scale[2] = sh->hz;
    in->pad = 0.0f;
    in->color[0] = sh->r;
    in->color[1] = sh->g;
    in->color[2] = sh->b;
    in->pad2 = 0.0f;
    (*count)++;
}

/*A box with its own orientation (the floating tome): the upper 3x3 of a
 * column-major 4x4 matrix.*/
static void push_oriented(GpdOriented *instances, uint32_t *count, uint32_t max_count,
                          const float pos[3], const Shape *sh, const float orient[16]) {
    if (!instances || *count >= max_count) {
        return;
    }
    GpdOriented *in = &instances[*count];
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, sizeof(in->pos));
    in->scale[0] = sh->hx;
    in->scale[1] = sh->hy;
    in->scale[2] = sh->hz;
    in->color[0] = sh->r;
    in->color[1] = sh->g;
    in->color[2] = sh->b;
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            in->orient[col * 3 + row] = orient[col * 4 + row];
        }
    }
    (*count)++;
}

/* --- the map ----------------------------------------------------------------- */

#define WALL(r, g, b)                                                                              \
    { 0.5f, 0.5f, 1.5f, 0.5f, r, g, b }
#define GROUND(r, g, b)                                                                            \
    { 0.0f, 0.5f, 0.1f, 0.5f, r, g, b }
#define LIQUID(r, g, b)                                                                            \
    { -0.05f, 0.5f, 0.05f, 0.5f, r, g, b }
#define CRYSTAL(r, g, b)                                                                           \
    { 0.8f, 0.4f, 0.8f, 0.4f, r, g, b }

/*How each tile is drawn: one box. A tile with hx == 0 (rock, anything
 * unknown) is not drawn.*/
static const Shape k_tile_shape[VOXEL_COUNT] = {
    [VOXEL_WALL] = WALL(0.6f, 0.6f, 0.6f),
    [VOXEL_OBSIDIAN] = WALL(0.1f, 0.05f, 0.2f),
    [VOXEL_GOLD_VEIN] = WALL(0.8f, 0.7f, 0.1f),
    [VOXEL_FLOOR] = GROUND(0.2f, 0.2f, 0.2f),
    [VOXEL_WOOD] = GROUND(0.4f, 0.3f, 0.2f),
    [VOXEL_COBBLE] = GROUND(0.3f, 0.3f, 0.3f),
    [VOXEL_ICE] = GROUND(0.6f, 0.8f, 1.0f),
    [VOXEL_SAND] = GROUND(0.8f, 0.7f, 0.4f),
    [VOXEL_ASH] = GROUND(0.25f, 0.25f, 0.25f),
    [VOXEL_MUD] = GROUND(0.3f, 0.2f, 0.1f),
    [VOXEL_MARBLE] = GROUND(0.9f, 0.9f, 0.9f),
    [VOXEL_GRASS] = GROUND(0.1f, 0.5f, 0.1f),
    [VOXEL_TRAP] = GROUND(0.8f, 0.2f, 0.1f),
    [VOXEL_WATER] = LIQUID(0.1f, 0.4f, 0.8f),
    [VOXEL_LAVA] = LIQUID(1.0f, 0.3f, 0.0f),
    [VOXEL_CRYSTAL_BLUE] = CRYSTAL(0.3f, 0.7f, 1.0f),
    [VOXEL_CRYSTAL_PURPLE] = CRYSTAL(0.8f, 0.2f, 1.0f),
    [VOXEL_CRYSTAL_RED] = CRYSTAL(1.0f, 0.1f, 0.1f),
    [VOXEL_CRYSTAL_GREEN] = CRYSTAL(0.1f, 1.0f, 0.2f),
    [VOXEL_CRYSTAL_YELLOW] = CRYSTAL(1.0f, 0.9f, 0.1f),
    [VOXEL_CRYSTAL_ORANGE] = CRYSTAL(1.0f, 0.5f, 0.0f),
    [VOXEL_CRYSTAL_CYAN] = CRYSTAL(0.0f, 0.9f, 1.0f),
    [VOXEL_CRYSTAL_WHITE] = CRYSTAL(1.0f, 1.0f, 1.0f),
    [VOXEL_DOOR] = {0.4f, 0.45f, 0.4f, 0.45f, 0.6f, 0.3f, 0.1f},
    [VOXEL_STAIRS_DOWN] = {0.05f, 0.5f, 0.1f, 0.5f, 0.9f, 0.9f, 0.0f},
    [VOXEL_STAIRS_UP] = {0.05f, 0.5f, 0.1f, 0.5f, 0.9f, 0.9f, 0.0f},
    [VOXEL_MUSHROOM_GLOW] = {0.2f, 0.3f, 0.2f, 0.3f, 0.2f, 1.0f, 0.5f},
};
/*The threshold a door stands on.*/
static const Shape k_door_sill = LIQUID(0.2f, 0.2f, 0.25f);

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    return v > hi ? hi : v;
}

/*The square of radius r around the player, inside the map.*/
static Window window_around(const FrameSnapshot *snap, int r) {
    Window w = {
        clamp_int(snap->my_x - r, 0, MAP_WIDTH - 1), clamp_int(snap->my_y - r, 0, MAP_HEIGHT - 1),
        clamp_int(snap->my_x + r, 0, MAP_WIDTH - 1), clamp_int(snap->my_y + r, 0, MAP_HEIGHT - 1)};
    return w;
}

static void forget_map(void) {
    s_map_built = false;
    s_map_gpd_count = 0;
    s_map_floor = -1;
    s_map_win.x1 = -1;
    (void)atomic_exchange_explicit(&g_map_dirty, false, memory_order_acq_rel);
}

/*Builds the instances of the tiles in win.*/
static void rebuild_map(const FrameSnapshot *snap, Window win) {
    uint32_t needed = (uint32_t)((win.x1 - win.x0 + 1) * (win.y1 - win.y0 + 1)) * 2u;
    if (!ensure_map_gpd_capacity(needed)) {
        fprintf(stderr, "[VK] Map GPD cache allocation failed\n");
        forget_map();
        return;
    }
    uint32_t count = 0;
    for (int y = win.y0; y <= win.y1; y++) {
        for (int x = win.x0; x <= win.x1; x++) {
            VoxelType tile = snap->map[y][x];
            if (tile >= VOXEL_COUNT || k_tile_shape[tile].hx == 0.0f) {
                continue;
            }
            push_instance(s_map_gpd_instances, &count, VKD_MAP_INST_MAX, GPD_BOX, (float)x,
                          (float)y, &k_tile_shape[tile]);
            if (tile == VOXEL_DOOR) {
                push_instance(s_map_gpd_instances, &count, VKD_MAP_INST_MAX, GPD_BOX, (float)x,
                              (float)y, &k_door_sill);
            }
        }
    }
    s_map_gpd_count = count;
    s_map_built = true;
    s_map_floor = snap->my_floor;
    s_map_win = win;
    (void)atomic_exchange_explicit(&g_map_dirty, false, memory_order_acq_rel);
}

/*Rebuilds the map window when the player left it, changed floor, or a
 * tile changed.*/
static void update_map(const FrameSnapshot *snap) {
    if (snap->my_x < 0 || snap->my_y < 0) {
        forget_map();
        return;
    }
    int r =
        clamp_int(snap->vision_radius + VK_MAP_VISION_MARGIN, VK_MAP_MIN_RADIUS, VK_MAP_MAX_RADIUS);
    Window need = window_around(snap, r);
    bool covers = s_map_built && s_map_floor == snap->my_floor && s_map_win.x0 <= need.x0 &&
                  s_map_win.x1 >= need.x1 && s_map_win.y0 <= need.y0 && s_map_win.y1 >= need.y1;
    if (atomic_load_explicit(&g_map_dirty, memory_order_acquire) || !covers) {
        rebuild_map(snap, window_around(snap, r + VK_MAP_BUILD_MARGIN));
    }
}

/* --- entities and particles ---------------------------------------------------- */

/*Colour of an entity: a merchant, the keeper of the martial archive,
 * another character, a boss-range id, anything else.*/
static void entity_color(const Entity *e, Shape *sh) {
    static const float k_rgb[5][3] = {{0.75f, 0.15f, 0.2f},
                                      {1.0f, 0.8f, 0.0f},
                                      {0.2f, 0.8f, 0.2f},
                                      {1.0f, 0.3f, 0.3f},
                                      {0.4f, 0.4f, 1.0f}};
    int k = 4;
    if (e->is_merchant && e->shop_spec == SHOP_SPEC_BOOKS_MARTIAL) {
        k = 0;
    } else if (e->is_merchant) {
        k = 1;
    } else if (e->is_player) {
        k = 2;
    } else if (e->id < 10) {
        k = 3;
    }
    sh->r = k_rgb[k][0];
    sh->g = k_rgb[k][1];
    sh->b = k_rgb[k][2];
}

static void push_entity(VkFrameResources *frame, uint32_t *dyn_count, uint32_t *ori_count, int i,
                        const FrameSnapshot *snap, float dt) {
    const Entity *e = &snap->entities[i];
    GpdInstance *dyn = (GpdInstance *)frame->gpd_dyn_ptr;
    float ex = g_entity_lerp[i].cur_x, ez = g_entity_lerp[i].cur_z;
    if (e->is_player) {
        Shape body = {0.4f, 0.35f, 0.5f, 0.35f, 0, 0, 0};
        entity_color(e, &body);
        push_instance(dyn, dyn_count, VKD_DYN_INST_MAX, GPD_PYRAMID, ex, ez, &body);
        return;
    }
    Shape body = {0.4f, 0.3f, 0.4f, 0.3f, 0, 0, 0};
    entity_color(e, &body);
    push_instance(dyn, dyn_count, VKD_DYN_INST_MAX, GPD_BOX, ex, ez, &body);
    if (e->is_merchant && e->shop_spec == SHOP_SPEC_BOOKS_MARTIAL) {
        static const Shape tome = {0.0f, 0.45f, 0.1f, 0.35f, 0.9f, 0.75f, 0.3f};
        float tp[3], to[16];
        tome_anim_update(i, e->id, e->floor_id, e->x, e->y, snap->map[0], dt, tp, to);
        push_oriented((GpdOriented *)frame->gpd_ori_ptr, ori_count, VKD_ORIENTED_MAX, tp, &tome,
                      to);
    }
}

static void push_particles(GpdInstance *dyn, uint32_t *dyn_count) {
    pthread_mutex_lock(&g_particles_mutex);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &g_particles[i];
        if (p->active) {
            Shape sh = {-p->y, p->size, p->size, p->size, p->r * p->a, p->g * p->a, p->b * p->a};
            push_instance(dyn, dyn_count, VKD_DYN_INST_MAX, GPD_PARTICLE, p->x, p->z, &sh);
        }
    }
    pthread_mutex_unlock(&g_particles_mutex);
}

void vk_scene_update(VkState *s, float dt, const FrameSnapshot *snap, bool particles) {
    VkFrameResources *frame = &s->frames[s->current_frame];
    update_map(snap);
    if (s_map_gpd_count > 0 && s_map_gpd_instances && frame->gpd_map_ptr) {
        memcpy(frame->gpd_map_ptr, s_map_gpd_instances,
               (size_t)s_map_gpd_count * sizeof(GpdInstance));
    }
    frame->gpd_map_count = s_map_gpd_count;

    uint32_t dyn_count = 0, ori_count = 0;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &snap->entities[i];
        if (e->active && e->id != snap->my_entity_id) {
            lerp_update(&g_entity_lerp[i], (float)e->x, (float)e->y, dt);
            if (e->floor_id == snap->my_floor) {
                push_entity(frame, &dyn_count, &ori_count, i, snap, dt);
            }
        } else if (!e->active && g_entity_lerp[i].initialized) {
            g_entity_lerp[i].initialized = false;
            tome_anim_reset_slot(i);
        }
    }
    /*The player's own character.*/
    static const Shape me = {0.6f, 0.3f, 0.6f, 0.3f, 0.0f, 1.0f, 0.0f};
    float px = (snap->my_x != -1) ? (float)snap->my_x : 500.0f;
    float pz = (snap->my_y != -1) ? (float)snap->my_y : 500.0f;
    push_instance((GpdInstance *)frame->gpd_dyn_ptr, &dyn_count, VKD_DYN_INST_MAX, GPD_BOX, px, pz,
                  &me);
    if (particles) {
        push_particles((GpdInstance *)frame->gpd_dyn_ptr, &dyn_count);
    }
    frame->gpd_dyn_count = dyn_count;
    frame->gpd_ori_count = ori_count;
}
