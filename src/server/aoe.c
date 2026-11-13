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
 * aoe.c - Areas of effect: geometry, terrain chemistry and clouds.
 *
 * The effect of a spell on each creature is resolved by spell_cast.c;
 * this file only answers "which cells", "what happens to the ground" and
 * "what stays on the map".
 */

#include "aoe.h"
#include "map.h"
#include "server_internal.h"
#include "server_world.h"
#include "spell_cast.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Geometry -------------------------------------------------------- */

static float dist_sq(int x1, int y1, int x2, int y2) {
    float dx = (float)(x1 - x2);
    float dy = (float)(y1 - y2);
    return dx * dx + dy * dy;
}

/*90-degree cone from the origin towards the target, `length` cells long.*/
static bool in_cone(const AoeArea *a, int px, int py) {
    float dx = (float)(a->tx - a->ox);
    float dy = (float)(a->ty - a->oy);
    float len = sqrtf(dx * dx + dy * dy);
    float ex = (float)(px - a->ox);
    float ey = (float)(py - a->oy);
    float pdist = sqrtf(ex * ex + ey * ey);
    if (len < 0.001f || pdist > (float)a->radius) {
        return false;
    }
    if (pdist < 0.001f) {
        return false; /*the caster's own cell*/
    }
    float cosangle = (dx * ex + dy * ey) / (len * pdist);
    return cosangle >= 0.707f; /*half-angle 45 degrees*/
}

/*Line one cell wide from the origin towards the target.*/
static bool on_line(const AoeArea *a, int px, int py) {
    float dx = (float)(a->tx - a->ox);
    float dy = (float)(a->ty - a->oy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return false;
    }
    float ex = (float)(px - a->ox);
    float ey = (float)(py - a->oy);
    float proj = (ex * dx + ey * dy) / len;
    if (proj <= 0.0f || proj > (float)a->radius) {
        return false;
    }
    float cross = fabsf(ex * dy - ey * dx) / len;
    return cross <= 0.75f;
}

bool aoe_contains(const AoeArea *a, int x, int y) {
    switch (a->shape) {
    case SPELL_TARGET_AOE_CIRCLE:
    case SPELL_TARGET_AOE_CLOUD:
        return dist_sq(a->ox, a->oy, x, y) <= (float)(a->radius * a->radius);
    case SPELL_TARGET_AOE_CONE:
        return in_cone(a, x, y);
    case SPELL_TARGET_AOE_LINE:
        return on_line(a, x, y);
    default:
        return x == a->ox && y == a->oy;
    }
}

/* --- Terrain chemistry ------------------------------------------------ */

/*What each damage type turns a voxel into.*/
static const struct {
    DamageType type;
    VoxelType from, to;
} k_reactions[] = {
    {DMG_FIRE, VOXEL_ICE, VOXEL_WATER},
    {DMG_FIRE, VOXEL_WOOD, VOXEL_ASH},
    {DMG_FIRE, VOXEL_WATER, VOXEL_MUD},
    {DMG_COLD, VOXEL_WATER, VOXEL_ICE},
    {DMG_COLD, VOXEL_LAVA, VOXEL_OBSIDIAN},
    {DMG_COLD, VOXEL_MUD, VOXEL_FLOOR},
    {DMG_ACID, VOXEL_WOOD, VOXEL_FLOOR},
    {DMG_ACID, VOXEL_DOOR, VOXEL_FLOOR},
    {DMG_FORCE, VOXEL_WALL, VOXEL_ROCK},
    {DMG_FORCE, VOXEL_COBBLE, VOXEL_ROCK},
    {DMG_FORCE, VOXEL_CRYSTAL_BLUE, VOXEL_FLOOR},
    {DMG_FORCE, VOXEL_CRYSTAL_PURPLE, VOXEL_FLOOR},
    {DMG_BLUDGEONING, VOXEL_WALL, VOXEL_ROCK},
    {DMG_BLUDGEONING, VOXEL_COBBLE, VOXEL_ROCK},
    {DMG_BLUDGEONING, VOXEL_CRYSTAL_BLUE, VOXEL_FLOOR},
    {DMG_BLUDGEONING, VOXEL_CRYSTAL_PURPLE, VOXEL_FLOOR},
};

static VoxelType react(VoxelType t, DamageType type) {
    for (size_t i = 0; i < sizeof(k_reactions) / sizeof(k_reactions[0]); i++) {
        if (k_reactions[i].type == type && k_reactions[i].from == t) {
            return k_reactions[i].to;
        }
    }
    return t;
}

void aoe_terraform(Game *game, const AoeArea *a, int floor_id, DamageType type) {
    if (!game->world || floor_id < 0 || floor_id >= MAX_FLOORS) {
        return;
    }
    Map *map = &game->world->floors[floor_id].map;
    int r = a->radius;
    for (int y = a->oy - r; y <= a->oy + r; y++) {
        for (int x = a->ox - r; x <= a->ox + r; x++) {
            if (x < 1 || x >= MAP_WIDTH - 1 || y < 1 || y >= MAP_HEIGHT - 1 ||
                !aoe_contains(a, x, y)) {
                continue;
            }
            map->data[0][y][x] = react(map->data[0][y][x], type);
        }
    }
}

void aoe_announce(const Client *caster, const SpellTemplate *sp) {
    const SpellEffect *e = &sp->effect;
    const char *what =
        e->kind == SPELL_KIND_DAMAGE ? damage_type_to_name(e->damage_type) : "arcane power";
    switch (e->target) {
    case SPELL_TARGET_AOE_CONE:
        send_text_to_client(caster->sock, "[ACTION] %s: a cone of %s surges forward over %d cells!",
                            sp->name, what, e->radius);
        break;
    case SPELL_TARGET_AOE_LINE:
        send_text_to_client(caster->sock, "[ACTION] %s: a line of %s cuts %d cells ahead!",
                            sp->name, what, e->radius);
        break;
    case SPELL_TARGET_AOE_CLOUD:
        send_text_to_client(caster->sock,
                            "[ACTION] %s: a cloud of %s spreads over %d cells and lingers for %d"
                            " rounds!",
                            sp->name, what, e->radius, e->rounds);
        break;
    default:
        send_text_to_client(caster->sock, "[ACTION] %s: %s bursts in a radius of %d cells!",
                            sp->name, what, e->radius);
        break;
    }
}

/* --- Clouds ------------------------------------------------------------ */

void aoe_init_clouds(Game *game) {
    memset(game->clouds, 0, sizeof(game->clouds));
}

bool aoe_spawn_cloud(Game *game, const SpellTemplate *sp, const SpellCaster *caster,
                     const char *owner, int floor_id, int cx, int cy) {
    for (int i = 0; i < MAX_CLOUDS; i++) {
        PersistentCloud *cl = &game->clouds[i];
        if (cl->active) {
            continue;
        }
        memset(cl, 0, sizeof(*cl));
        cl->active = true;
        cl->cx = cx;
        cl->cy = cy;
        cl->floor_id = floor_id;
        cl->rounds_left = sp->effect.rounds - 1; /*the cast itself is the first round*/
        if (cl->rounds_left <= 0) {
            cl->active = false;
            return true;
        }
        cl->spell = sp;
        cl->caster = *caster;
        if (owner) {
            copy_str(cl->owner, owner, sizeof(cl->owner));
        }
        return true;
    }
    return false;
}

/*The caster of the cloud, if still online.*/
static Client *cloud_owner(const PersistentCloud *cl, Client *clients, int client_count) {
    for (int i = 0; cl->owner[0] && i < client_count; i++) {
        if (clients[i].active && clients[i].authenticated &&
            strcmp(clients[i].username, cl->owner) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

static void cloud_round(PersistentCloud *cl, Game *game, Client *owner) {
    AoeArea area = {
        SPELL_TARGET_AOE_CLOUD, cl->spell->effect.radius, cl->cx, cl->cy, cl->cx, cl->cy};
    FloorIter walk = floor_iter(game, cl->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        NPC *n = &game->npcs[i];
        if (spell_can_affect_npc(n) && aoe_contains(&area, n->x, n->y)) {
            spell_affect_npc(game, cl->spell, &cl->caster, n, owner, 1);
        }
    }
}

void aoe_update_clouds(Game *game, Client *clients, int client_count) {
    for (int ci = 0; ci < MAX_CLOUDS; ci++) {
        PersistentCloud *cl = &game->clouds[ci];
        if (!cl->active) {
            continue;
        }
        Client *owner = cloud_owner(cl, clients, client_count);
        cloud_round(cl, game, owner);
        cl->rounds_left--;
        if (cl->rounds_left <= 0) {
            cl->active = false;
            if (owner) {
                send_text_to_client(owner->sock, "[ENVIRONMENT] Your %s dissolves.",
                                    cl->spell->name);
            }
        }
    }
}
