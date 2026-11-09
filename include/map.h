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

#ifndef MAP_H
#define MAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "traps.h"

#define MAP_WIDTH           300
#define MAP_HEIGHT          300
#define MAP_DEPTH           1
#define MAX_FLOORS          101
#define MAX_TRAPS_PER_FLOOR 100

#define MAP_CENTER_X        (MAP_WIDTH / 2)
#define MAP_CENTER_Y        (MAP_HEIGHT / 2)
#define INITIAL_VIEW_RADIUS 100

/*Logical voxel identifiers (33 types). Kept as an enum for readable names;
 * the values MUST stay in the uint8_t range because maps are stored with
 * VoxelType (1 byte/tile: 360 KB per floor instead of 1.4 MB, world.dat
 * 19 MB instead of 73 MB, map chunks on the wire 4x smaller).*/
typedef enum {
    VOXEL_ROCK = 0,
    VOXEL_FLOOR = 1,
    VOXEL_WALL = 2,
    VOXEL_DOOR = 3,
    VOXEL_STAIRS_UP = 4,
    VOXEL_STAIRS_DOWN = 5,
    VOXEL_GRASS = 6,
    VOXEL_WOOD = 7,
    VOXEL_WATER = 8,
    VOXEL_COBBLE = 9,
    VOXEL_TRAP = 10,
    VOXEL_LAVA = 11,
    VOXEL_ICE = 12,
    VOXEL_SAND = 13,
    VOXEL_ASH = 14,
    VOXEL_MUD = 15,
    VOXEL_MARBLE = 16,
    VOXEL_MUSHROOM_GLOW = 17,
    VOXEL_CRYSTAL_BLUE = 18,
    VOXEL_CRYSTAL_PURPLE = 19,
    VOXEL_GOLD_VEIN = 20,
    VOXEL_OBSIDIAN = 21,
    VOXEL_CRYSTAL_RED = 22,
    VOXEL_CRYSTAL_GREEN = 23,
    VOXEL_CRYSTAL_YELLOW = 24,
    VOXEL_CRYSTAL_ORANGE = 25,
    VOXEL_CRYSTAL_CYAN = 26,
    VOXEL_CRYSTAL_WHITE = 27,
    VOXEL_COUNT = 28
} VoxelId;

/*Compact storage type: 1 byte per tile. All VoxelId values fit (checked
 * below), so every existing comparison/assignment keeps its meaning.*/
typedef uint8_t VoxelType;

_Static_assert(VOXEL_COUNT <= 256, "VoxelId values must fit in a uint8_t (VoxelType)");

typedef VoxelType TileType;
#define TILE_EMPTY VOXEL_FLOOR
#define TILE_WALL  VOXEL_WALL
#define TILE_DOOR  VOXEL_DOOR
#define TILE_ROCK  VOXEL_ROCK

typedef struct {
    VoxelType data[MAP_DEPTH][MAP_HEIGHT][MAP_WIDTH];
} Map;

typedef struct {
    int x, y;
    VoxelType type;
    int respawn_timer;
} CrystalRespawn;

typedef struct {
    int id;
    Map map;
    Trap traps[MAX_TRAPS_PER_FLOOR];
    int trap_count;
    int entity_grid[MAP_HEIGHT][MAP_WIDTH]; // stores entity_id, 0 = empty
    CrystalRespawn crystal_respawns[100];
    int crystal_respawn_count;
} Floor;

typedef struct {
    Floor floors[MAX_FLOORS];
} World;

/* Generate every floor of the world (floor_init on each). */
void world_init(World *world);
/* Generate one floor: the city on floor 0, a dungeon below, and its traps.
 * Uses rand(): the caller seeds the RNG. */
void floor_init(Floor *floor, int floor_id);
void map_dig_room(Map *map, int x, int y, int w, int h);
void generate_procedural_dungeon(Map *map, int floor_id);

/* Angle in radians of shop slot 0..10 on the ring of floor 0 (map.c,
 * generate_city). The server places the shop doors and merchants with the
 * same angle, so the computation lives here only. */
float city_shop_angle(int slot);

#endif
