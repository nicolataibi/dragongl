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
 * client_minimap.h — Overlay Radar Minimap
 *
 * Module independent of the graphics backend.
 * Maintains a compressed copy of the local map explored by the player
 * and exposes it as a pixel buffer (RGBA) to be rendered.
 *
 * Performance: The internal map is a static MAP_WIDTH × MAP_HEIGHT array.
 * The update occurs only when the player's position changes
 * (not every frame), minimizing the CPU cost.
 */

#ifndef CLIENT_MINIMAP_H
#define CLIENT_MINIMAP_H

#include <stdbool.h>
#include "map.h"

/*Minimap tile size in pixels per screen*/
#define MINIMAP_DISPLAY_SIZE 160

/*Minimap viewing range (how many player tiles to show)*/
#define MINIMAP_RADIUS 40

/**
 * minimap_update — Updates the internal texture of the minimap.
 *
 * @param player_x Player's X position on the map.
 * @param player_y Y position of the player in the map.
 * @param map Reference to the local map (g_local_map).
 * @param vision_r Vision range of the player.
 *
 * Call every frame (or when the player moves).
 * The function internally checks whether the position has changed
 * and rebuilds the buffer only if necessary.*/
void minimap_update(int player_x,
                    int player_y,
                    TileType map[MAP_HEIGHT][MAP_WIDTH],
                    int vision_r);

/**
 * minimap_get_explored — Returns true if cell (x,y) has been
 * explored at least once by the player.*/
bool minimap_get_explored(int x, int y);

/**
 * minimap_mark_explored — Marks an area as explored.
 * Called internally by minimap_update.*/
void minimap_mark_explored(int cx, int cy, int radius);

/**
 * minimap_reset — Reset the explored map (switch plane).*/
void minimap_reset(void);

/*────────────────────────────────── ──────────────────────────────────
 * Internal data exposed to renderers for drawing:
 * The pixel buffer contains the RGBA colors of the minimap.
 * ────────────────────────────────── ──────────────────────────────────*/

/*Pixel buffer size (visible area of ​​the mini-map)*/
#define MINIMAP_BUF_SIZE (MINIMAP_RADIUS * 2 + 1)

typedef struct {
    unsigned char r, g, b, a;
} MiniPixel;

/*Minimap pixel buffer — read by renderers*/
extern MiniPixel g_minimap_buf[MINIMAP_BUF_SIZE][MINIMAP_BUF_SIZE];

/* Flag: if true, the buffer was updated and the renderer must redraw */
extern bool g_minimap_dirty;

#endif /* CLIENT_MINIMAP_H */
