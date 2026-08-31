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
 * client_minimap.c — Radar Minimap — Implementation
 *
 * Handles progressive "fog of war" and generates a pixel buffer
 * readable by OpenGL/Vulkan renderers to draw radar.
 *
 * Performance:
 * - Static array explored[][] (MAP_WIDTH × MAP_HEIGHT bool) ~64KB.
 * - Pixel buffer g_minimap_buf[][] recalculated ONLY when the
 * player moves (cache position check).
 * - No dynamic allocation.*/

#include "client_minimap.h"
#include <string.h>
#include <math.h>

/*Global Array: Minimap pixel buffer*/
MiniPixel g_minimap_buf[MINIMAP_BUF_SIZE][MINIMAP_BUF_SIZE] = {0};
bool g_minimap_dirty = false;

/*Persistent exploration map per floor*/
static bool s_explored[MAP_HEIGHT][MAP_WIDTH] = {{false}};

/*Previous position — to avoid unnecessary recalculations*/
static int s_last_px = -1;
static int s_last_py = -1;

/*----------------------------------------------------------------
 * minimap_reset — Resets all fog of war (plane change).
 * ----------------------------------------------------------------*/
void minimap_reset(void) {
    memset(s_explored, 0, sizeof(s_explored));
    memset(g_minimap_buf, 0, sizeof(g_minimap_buf));
    s_last_px = -1;
    s_last_py = -1;
    g_minimap_dirty = true;
}

/*----------------------------------------------------------------
* minimap_get_explored — Checks whether a cell has been explored.
 * ----------------------------------------------------------------*/
bool minimap_get_explored(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) {
        return false;
    }
    return s_explored[y][x];
}

/*----------------------------------------------------------------
* minimap_mark_explored — Marks a circle as explored.
 * ----------------------------------------------------------------*/
void minimap_mark_explored(int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            int mx = cx + dx;
            int my = cy + dy;
            if (mx >= 0 && mx < MAP_WIDTH && my >= 0 && my < MAP_HEIGHT) {
                s_explored[my][mx] = true;
            }
        }
    }
}

/*----------------------------------------------------------------
 * tile_to_color — Converts a voxel type to a minimap color.
 * ----------------------------------------------------------------*/
static MiniPixel tile_to_color(TileType t) {
    MiniPixel c = {0, 0, 0, 255};
    switch (t) {
        case VOXEL_FLOOR:
            c.r = 40;
            c.g = 40;
            c.b = 50;
            break;
        case VOXEL_WALL:
            c.r = 120;
            c.g = 120;
            c.b = 130;
            break;
        case VOXEL_OBSIDIAN:
            c.r = 30;
            c.g = 15;
            c.b = 60;
            break;
        case VOXEL_DOOR:
            c.r = 130;
            c.g = 80;
            c.b = 30;
            break;
        case VOXEL_STAIRS_DOWN:
            c.r = 230;
            c.g = 230;
            c.b = 0;
            break;
        case VOXEL_STAIRS_UP:
            c.r = 0;
            c.g = 230;
            c.b = 230;
            break;
        case VOXEL_WATER:
            c.r = 20;
            c.g = 80;
            c.b = 200;
            break;
        case VOXEL_LAVA:
            c.r = 255;
            c.g = 60;
            c.b = 0;
            break;
        case VOXEL_GRASS:
            c.r = 20;
            c.g = 100;
            c.b = 20;
            break;
        case VOXEL_ICE:
            c.r = 150;
            c.g = 200;
            c.b = 255;
            break;
        case VOXEL_SAND:
            c.r = 200;
            c.g = 180;
            c.b = 100;
            break;
        case VOXEL_GOLD_VEIN:
            c.r = 200;
            c.g = 180;
            c.b = 20;
            break;
        case VOXEL_CRYSTAL_BLUE:
            c.r = 80;
            c.g = 180;
            c.b = 255;
            break;
        case VOXEL_CRYSTAL_PURPLE:
            c.r = 200;
            c.g = 50;
            c.b = 255;
            break;
        case VOXEL_MUSHROOM_GLOW:
            c.r = 50;
            c.g = 255;
            c.b = 130;
            break;
        case VOXEL_TRAP:
            c.r = 200;
            c.g = 50;
            c.b = 30;
            break;
        case VOXEL_WOOD:
            c.r = 100;
            c.g = 75;
            c.b = 50;
            break;
        case VOXEL_COBBLE:
            c.r = 75;
            c.g = 75;
            c.b = 75;
            break;
        case VOXEL_MUD:
            c.r = 75;
            c.g = 50;
            c.b = 25;
            break;
        case VOXEL_MARBLE:
            c.r = 230;
            c.g = 230;
            c.b = 230;
            break;
        case VOXEL_ASH:
            c.r = 65;
            c.g = 65;
            c.b = 65;
            break;
        case VOXEL_ROCK:
        default:
            c.r = 0;
            c.g = 0;
            c.b = 0;
            c.a = 0; /*completely transparent = not explored*/
            break;
    }
    return c;
}

/* ----------------------------------------------------------------
 * minimap_update — Updates exploration and pixel buffer.
 * ---------------------------------------------------------------- */
void minimap_update(int player_x,
                    int player_y,
                    TileType map[MAP_HEIGHT][MAP_WIDTH],
                    int vision_r) {
    /*Mark visible cells as explored*/
    minimap_mark_explored(player_x, player_y, vision_r);

    /* If the player hasn't moved, skip recalculating the buffer */
    if (player_x == s_last_px && player_y == s_last_py) {
        return;
    }
    s_last_px = player_x;
    s_last_py = player_y;

    /*Rebuild the player-centered pixel buffer*/
    for (int dy = -MINIMAP_RADIUS; dy <= MINIMAP_RADIUS; dy++) {
        for (int dx = -MINIMAP_RADIUS; dx <= MINIMAP_RADIUS; dx++) {
            int buf_x = dx + MINIMAP_RADIUS;
            int buf_y = dy + MINIMAP_RADIUS;
            int map_x = player_x + dx;
            int map_y = player_y + dy;

            /*Outside the limits of the map → transparent black*/
            if (map_x < 0 || map_x >= MAP_WIDTH ||
                map_y < 0 || map_y >= MAP_HEIGHT) {
                g_minimap_buf[buf_y][buf_x] = (MiniPixel){0, 0, 0, 0};
                continue;
            }

            /*Not yet explored → matte black (fog of war)*/
            if (!s_explored[map_y][map_x]) {
                g_minimap_buf[buf_y][buf_x] = (MiniPixel){10, 10, 15, 200};
                continue;
            }

            /*Cell explored → color based on type*/
            MiniPixel color = tile_to_color(map[map_y][map_x]);

            /*Slightly blurs cells away from the player*/
            float dist = sqrtf((float)(dx * dx + dy * dy));
            float fog = 1.0f;
            if (dist > (float)vision_r) {
                fog = 0.5f; /*area explored but out of current view*/
            }
            color.r = (unsigned char)((float)color.r * fog);
            color.g = (unsigned char)((float)color.g * fog);
            color.b = (unsigned char)((float)color.b * fog);

            g_minimap_buf[buf_y][buf_x] = color;
        }
    }

    g_minimap_dirty = true;
}
