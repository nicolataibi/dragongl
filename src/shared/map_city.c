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

/*map_city.c - Floor 0: the city.*/

#include "map_internal.h"
#include <math.h>

float city_shop_angle(int slot) {
    return (float)((float)slot * (360.0f / 11.0f) * (M_PI / 180.0f));
}

/*The circular square: fountain, its ice rim, a cobble and marble pattern,
 * a marble border.*/
static void city_square(Map *map, int cx, int cy) {
    enum { R_MARBLE = 36, R_COBBLE = 34, R_WATER = 9 };
    for (int y = cy - R_MARBLE; y <= cy + R_MARBLE; y++) {
        for (int x = cx - R_MARBLE; x <= cx + R_MARBLE; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist <= (float)R_WATER) {
                map->data[0][y][x] = VOXEL_WATER;
            } else if (dist <= (float)R_WATER + 2.0f) {
                map->data[0][y][x] = VOXEL_ICE;
            } else if (dist <= (float)R_COBBLE) {
                map->data[0][y][x] = ((int)dist) % 4 == 0 ? VOXEL_MARBLE : VOXEL_COBBLE;
            } else if (dist <= (float)R_MARBLE) {
                map->data[0][y][x] = VOXEL_MARBLE;
            }
        }
    }
    //Decorations: Crystals at the corners of the fountain
    map_fill_rect(map, cx - 11, cy - 11, 2, 2, VOXEL_CRYSTAL_BLUE);
    map_fill_rect(map, cx + 10, cy - 11, 2, 2, VOXEL_CRYSTAL_BLUE);
    map_fill_rect(map, cx - 11, cy + 10, 2, 2, VOXEL_CRYSTAL_BLUE);
    map_fill_rect(map, cx + 10, cy + 10, 2, 2, VOXEL_CRYSTAL_BLUE);
}

/*The temple of a class: an 11x11 rock building (corner at x, y) with a
 * 9x9 floor, a 3x3 crystal altar and a door towards the square; its statue
 * (3x3 crystal) and its fountain (5x5 border, 3x3 water) further out.
 *
 * The eight casters stand on the inner ring (r=44, statues and fountains
 * around r=60); the four martial classes on the cardinal points of the
 * outer ring (temple r=78, statue r=90, fountain r=97).*/
typedef struct {
    int x, y;
    VoxelType floor, crystal;
    int door_x, door_y;
    int statue_x, statue_y;
    int fountain_x, fountain_y;
    VoxelType fountain_border;
} CityTemple;

static const CityTemple k_temples[] = {
    /*Wizard (N)*/ {145, 101, VOXEL_MARBLE, VOXEL_CRYSTAL_BLUE, 150, 111, 149, 84, 148, 89,
                    VOXEL_MARBLE},
    /*Paladin (NE)*/
    {176, 113, VOXEL_MARBLE, VOXEL_CRYSTAL_YELLOW, 181, 123, 194, 103, 189, 106, VOXEL_COBBLE},
    /*Cleric (E)*/
    {189, 145, VOXEL_MARBLE, VOXEL_CRYSTAL_WHITE, 189, 150, 214, 149, 207, 148, VOXEL_ICE},
    /*Sorcerer (SE)*/
    {176, 176, VOXEL_MARBLE, VOXEL_CRYSTAL_RED, 176, 181, 194, 194, 189, 189, VOXEL_OBSIDIAN},
    /*Warlock (S)*/
    {145, 189, VOXEL_MARBLE, VOXEL_CRYSTAL_PURPLE, 150, 189, 149, 214, 148, 207, VOXEL_ASH},
    /*Bard (SW)*/
    {113, 176, VOXEL_WOOD, VOXEL_CRYSTAL_ORANGE, 123, 181, 103, 194, 106, 189, VOXEL_WOOD},
    /*Druid (W)*/
    {101, 145, VOXEL_GRASS, VOXEL_CRYSTAL_GREEN, 111, 150, 84, 149, 89, 148, VOXEL_SAND},
    /*Ranger (NW)*/
    {113, 113, VOXEL_WOOD, VOXEL_CRYSTAL_CYAN, 123, 118, 103, 103, 106, 106, VOXEL_GOLD_VEIN},
    /*Fighter: Gladiator's Arena (N)*/
    {145, 67, VOXEL_COBBLE, VOXEL_CRYSTAL_ORANGE, 150, 77, 149, 59, 148, 51, VOXEL_MARBLE},
    /*Barbarian: Fighting Pit (E)*/
    {223, 145, VOXEL_GRASS, VOXEL_CRYSTAL_RED, 223, 150, 239, 149, 245, 148, VOXEL_COBBLE},
    /*Rogue: Den of Shadows (S)*/
    {145, 223, VOXEL_WOOD, VOXEL_CRYSTAL_PURPLE, 150, 223, 149, 239, 148, 245, VOXEL_ASH},
    /*Monk: Lotus Dojo (W)*/
    {67, 145, VOXEL_WOOD, VOXEL_CRYSTAL_WHITE, 77, 150, 59, 149, 51, 148, VOXEL_MARBLE},
};

static void city_temples(Map *map) {
    for (size_t i = 0; i < sizeof(k_temples) / sizeof(k_temples[0]); i++) {
        const CityTemple *t = &k_temples[i];
        map_fill_rect(map, t->x, t->y, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, t->x + 1, t->y + 1, 9, 9, t->floor);
        map_fill_rect(map, t->x + 4, t->y + 4, 3, 3, t->crystal);
        map->data[0][t->door_y][t->door_x] = VOXEL_DOOR;
        map_fill_rect(map, t->statue_x, t->statue_y, 3, 3, t->crystal);
        map_fill_rect(map, t->fountain_x, t->fountain_y, 5, 5, t->fountain_border);
        map_fill_rect(map, t->fountain_x + 1, t->fountain_y + 1, 3, 3, VOXEL_WATER);
    }
}

/*The 11 shops on a circle of radius 26: slot i at angle i * (360/11)
 * degrees. Slot 10 is The Archive of a Thousand Battles (martial bookshop,
 * see spawn_martial_archive in server_spawn.c). Each is a 9x9 rock
 * building with a 7x7 wooden floor, the merchant's chest and a door on the
 * side facing the square.*/
static void city_shops(Map *map, int cx, int cy) {
    for (int i = 0; i < 11; i++) {
        float angle = city_shop_angle(i);
        int sx = cx + (int)(cosf(angle) * 26.0f);
        int sy = cy + (int)(sinf(angle) * 26.0f);
        map_fill_rect(map, sx - 4, sy - 4, 9, 9, VOXEL_ROCK);
        map_fill_rect(map, sx - 3, sy - 3, 7, 7, VOXEL_WOOD);
        map_fill_rect(map, sx - 1, sy - 1, 3, 2, VOXEL_ASH);

        float dx = (float)(cx - sx);
        float dy = (float)(cy - sy);
        if (fabsf(dx) > fabsf(dy)) {
            map->data[0][sy][dx > 0 ? sx + 4 : sx - 4] = VOXEL_DOOR;
        } else {
            map->data[0][dy > 0 ? sy + 4 : sy - 4][sx] = VOXEL_DOOR;
        }
    }
}

/*Floor 0: the city. The square and its fountain, the class temples, the
 * ring of 11 shops (the server computes their doors the same way, see
 * update_city_doors) and the stairs down.*/
void generate_city(Map *map) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;

    //General grass background
    map_fill_rect(map, 0, 0, MAP_WIDTH, MAP_HEIGHT, VOXEL_GRASS);

    //Paths to the cardinal points (leaving the square)
    map_fill_rect(map, cx - 3, 0, 7, MAP_HEIGHT, VOXEL_MARBLE);
    map_fill_rect(map, 0, cy - 3, MAP_WIDTH, 7, VOXEL_MARBLE);
    map_fill_rect(map, cx - 2, 0, 5, MAP_HEIGHT, VOXEL_COBBLE);
    map_fill_rect(map, 0, cy - 2, MAP_WIDTH, 5, VOXEL_COBBLE);

    city_square(map, cx, cy);
    city_temples(map);
    city_shops(map, cx, cy);

    //Central island safe for stairs, the stairs in the exact center
    map_fill_rect(map, cx - 2, cy - 2, 5, 5, VOXEL_MARBLE);
    map_fill_rect(map, cx - 1, cy - 1, 3, 3, VOXEL_COBBLE);
    map->data[0][cy][cx] = VOXEL_STAIRS_DOWN;
}
