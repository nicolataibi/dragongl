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

/*map_traps.c - The traps a new floor is born with.*/

#include "map_internal.h"
#include <stdlib.h>

/*Voxels a floor trap can sit on (and a wall trap can fire onto).*/
static bool trap_floor_voxel(VoxelType v) {
    return v == VOXEL_FLOOR || v == VOXEL_WOOD || v == VOXEL_COBBLE || v == VOXEL_SAND ||
           v == VOXEL_MUD || v == VOXEL_MARBLE || v == VOXEL_ASH;
}

/*Voxels that can hide a dart tube or a spring spear.*/
static bool trap_wall_voxel(VoxelType v) {
    return v == VOXEL_WALL || v == VOXEL_COBBLE || v == VOXEL_ROCK || v == VOXEL_OBSIDIAN ||
           v == VOXEL_ICE || v == VOXEL_MARBLE;
}

static bool trap_at(const Floor *f, int x, int y) {
    for (int j = 0; j < f->trap_count; j++) {
        if (f->traps[j].x == x && f->traps[j].y == y) {
            return true;
        }
    }
    return false;
}

/*A new armed, undetected trap; the caller sets its DCs and damage.*/
static Trap *trap_add(Floor *f, TrapType type, int x, int y, int wall_dir) {
    Trap *t = &f->traps[f->trap_count++];
    t->type = type;
    t->x = x;
    t->y = y;
    t->floor_id = f->id;
    t->active = true;
    t->detected = false;
    t->respawn_timer = 0;
    t->wall_dir = wall_dir;
    return t;
}

/*Damage of a floor trap by type; the default is 1d6 (+1 die every 20
 * floors) bludgeoning.*/
static void floor_trap_damage(Trap *t, int floor_id) {
    t->damage_dice = 1 + (floor_id / 20);
    t->damage_sides = 6;
    t->damage_type = DMG_BLUDGEONING;
    switch (t->type) {
    case TRAP_SPIKES:
        t->damage_type = DMG_PIERCING;
        break;
    case TRAP_POISON_NEEDLE:
    case TRAP_GAS:
    case TRAP_SLEEP_GAS:
    case TRAP_PARALYZING_SPORES:
        t->damage_type = DMG_POISON;
        t->damage_sides = 4;
        break;
    case TRAP_ACID:
        t->damage_type = DMG_ACID;
        break;
    case TRAP_LIGHTNING:
        t->damage_type = DMG_LIGHTNING;
        break;
    case TRAP_BOULDER:
    case TRAP_CRUSHER_WALL:
        t->damage_dice = 2 + (floor_id / 15);
        t->damage_sides = 8;
        break;
    case TRAP_HUNGER_CURSE:
    case TRAP_CURSE_RUNE:
        t->damage_type = DMG_NECROTIC;
        t->damage_dice = 1;
        t->damage_sides = 4;
        break;
    default:
        break;
    }
}

/*Traps on random walkable tiles, of a random type of the floor pool.*/
static void spawn_floor_traps(Floor *f, int count) {
    /*Floor trap pool (excludes wall-based and tile-special)*/
    static const TrapType floor_pool[] = {
        TRAP_PIT,           TRAP_SPIKES,         TRAP_POISON_NEEDLE, TRAP_GAS,
        TRAP_ALARM,         TRAP_TELEPORT,       TRAP_ACID,          TRAP_WEB,
        TRAP_BLADE,         TRAP_SILENCE,        TRAP_LIGHTNING,     TRAP_QUICKSAND,
        TRAP_DARKNESS,      TRAP_FALLING_FLOOR,  TRAP_BOULDER,       TRAP_PARALYZING_SPORES,
        TRAP_CURSE_RUNE,    TRAP_ANTIMAGIC_ZONE, TRAP_SLEEP_GAS,     TRAP_ILLUSION_MIRROR,
        TRAP_SWAP_TELEPORT, TRAP_FAKE_DOOR,      TRAP_FLOOD,         TRAP_CEILING_COLLAPSE,
        TRAP_CRUSHER_WALL,  TRAP_INVERSION_RUNE, TRAP_STONE_TOMB,    TRAP_HUNGER_CURSE,
        TRAP_BODY_SWAP};
    int pool_size = (int)(sizeof(floor_pool) / sizeof(floor_pool[0]));
    int floor_bonus = f->id / 10;

    for (int i = 0; i < count; i++) {
        for (int attempts = 0; attempts < 100; attempts++) {
            int tx = rand() % MAP_WIDTH;
            int ty = rand() % MAP_HEIGHT;
            if (!trap_floor_voxel(f->map.data[0][ty][tx]) || trap_at(f, tx, ty)) {
                continue;
            }
            Trap *t = trap_add(f, floor_pool[rand() % pool_size], tx, ty, 0);
            t->detection_dc = 10 + (rand() % 5) + floor_bonus;
            t->save_dc = 10 + (rand() % 5) + floor_bonus;
            floor_trap_damage(t, f->id);
            break;
        }
    }
}

/*A kind of trap hidden in a wall tile that faces a walkable tile.*/
typedef struct {
    TrapType type;
    int count, max_attempts;
    bool onto_cobble;      /*may fire onto a cobble tile*/
    int detection_dc_base; /*+ 0..3 + floor / 10*/
    int save_dc_base;      /*+ 0..4 + floor / 10*/
    int dice_floors;       /*1 die, +1 every this many floors*/
    int sides;
} WallTrapKind;

static void spawn_wall_traps(Floor *f, const WallTrapKind *k) {
    /*N, E, S, W: the side of the wall the trap fires from*/
    static const int wall_dx[4] = {0, 1, 0, -1};
    static const int wall_dy[4] = {-1, 0, 1, 0};
    int floor_bonus = f->id / 10;
    int placed = 0;
    for (int attempts = 0; placed < k->count && attempts < k->max_attempts; attempts++) {
        int tx = 5 + rand() % (MAP_WIDTH - 10);
        int ty = 5 + rand() % (MAP_HEIGHT - 10);
        if (!trap_wall_voxel(f->map.data[0][ty][tx])) {
            continue;
        }
        int dir = rand() % 4;
        int nx = tx + wall_dx[dir];
        int ny = ty + wall_dy[dir];
        if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
            continue;
        }
        VoxelType nv = f->map.data[0][ny][nx];
        if (!trap_floor_voxel(nv) || (nv == VOXEL_COBBLE && !k->onto_cobble) ||
            trap_at(f, tx, ty)) {
            continue;
        }
        if (f->trap_count >= MAX_TRAPS_PER_FLOOR) {
            break;
        }
        Trap *t = trap_add(f, k->type, tx, ty, dir);
        t->damage_type = DMG_PIERCING;
        t->detection_dc = k->detection_dc_base + (rand() % 4) + floor_bonus;
        t->save_dc = k->save_dc_base + (rand() % 5) + floor_bonus;
        t->damage_dice = 1 + (f->id / k->dice_floors);
        t->damage_sides = k->sides;
        placed++;
    }
}

/*A kind of trap that sits on every tile of two voxel types with a chance.*/
typedef struct {
    TrapType type;
    VoxelType on_a, on_b;
    int chance_pct;
    int max; /*0 = no limit*/
    DamageType damage_type;
    int detection_dc_base; /*+ floor / 10*/
    int sides;
} VoxelTrapKind;

static void spawn_voxel_traps(Floor *f, const VoxelTrapKind *k) {
    int placed = 0;
    for (int y = 5; y < MAP_HEIGHT - 5 && f->trap_count < MAX_TRAPS_PER_FLOOR; y++) {
        for (int x = 5; x < MAP_WIDTH - 5 && f->trap_count < MAX_TRAPS_PER_FLOOR; x++) {
            VoxelType tv = f->map.data[0][y][x];
            if ((tv != k->on_a && tv != k->on_b) || rand() % 100 >= k->chance_pct ||
                (k->max > 0 && placed >= k->max) || trap_at(f, x, y)) {
                continue;
            }
            Trap *t = trap_add(f, k->type, x, y, 0);
            t->damage_type = k->damage_type;
            t->detection_dc = k->detection_dc_base + (f->id / 10);
            t->save_dc = 12 + (f->id / 10);
            t->damage_dice = 2 + (f->id / 20);
            t->damage_sides = k->sides;
            placed++;
        }
    }
}

static int at_most(int v, int max) {
    return v > max ? max : v;
}

void spawn_traps_on_floor(Floor *f) {
    if (f->id == 0) {
        return; // No traps in the city
    }
    f->trap_count = 0;
    spawn_floor_traps(f, at_most(10 + (rand() % 15) + (f->id / 5), MAX_TRAPS_PER_FLOOR - 20));

    /*Dart tubes (1d4 piercing + poison) and spring spears in the walls*/
    WallTrapKind dart = {
        TRAP_DART_WALL, at_most(3 + (rand() % 4) + (f->id / 15), 12), 5000, true, 12, 11, 25, 4};
    spawn_wall_traps(f, &dart);
    WallTrapKind spear = {
        TRAP_SPRING_SPEAR, at_most(2 + (rand() % 3) + (f->id / 20), 8), 3000, false, 13, 12, 20, 8};
    spawn_wall_traps(f, &spear);

    /*Crystals that burst, gas veins under ash and mud*/
    static const VoxelTrapKind crystal = {
        TRAP_CRYSTAL_BURST, VOXEL_CRYSTAL_BLUE, VOXEL_CRYSTAL_PURPLE, 40, 0, DMG_PIERCING, 14, 4};
    spawn_voxel_traps(f, &crystal);
    static const VoxelTrapKind gas_vein = {TRAP_GAS_VEIN, VOXEL_ASH, VOXEL_MUD, 20, 8,
                                           DMG_FIRE,      15,        6};
    spawn_voxel_traps(f, &gas_vein);
}
