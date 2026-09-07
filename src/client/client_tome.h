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

#ifndef CLIENT_TOME_H
#define CLIENT_TOME_H

#include <stdbool.h>
#include "map.h"

/*
 * Animation of the floating tome of the merchant of "The Archive of a
 * Thousand Battles" (SHOP_SPEC_BOOKS_MARTIAL): the book wanders a RANDOM
 * trajectory around its owner while remaining INSIDE the shop room, and
 * performs a RANDOM rotation on itself while it flies.
 *
 * The state lives here (not in render_gl.c / render_vk.c) so BOTH render
 * backends animate the exact same trajectory/rotation: each renderer calls
 * tome_anim_update() once per frame for the merchant's entity slot and then
 * draws the book at the returned position with the returned orientation.
 *
 * Slots are indexed exactly like g_entities[] / g_entity_lerp[] in the
 * renderers. Only the render thread touches this state (never the net
 * thread), so no locking is needed.
 */

/* Invalidate the animation of every slot (e.g. on reconnect). */
void tome_anim_reset_all(void);

/* Invalidate the animation of a single slot (entity became inactive). */
void tome_anim_reset_slot(int slot);

/*
 * Advance the animation of the entity currently stored in `slot` by dt
 * seconds and output the tome transform.
 *
 * (mx, my)      : merchant integer tile coordinates (absolute map coords).
 * map           : current local map, TileType[MAP_HEIGHT][MAP_WIDTH]
 *                 (the FrameSnapshot copy is fine; only the neighbourhood
 *                 of the merchant is read).
 * out_pos[3]    : tome center in ABSOLUTE world tile coordinates
 *                 (y = height above the floor, in tiles).
 * out_orient[16]: column-major 4x4 rotation matrix of the tome
 *                 (same layout as glMultMatrixf / the VK mvp push constant).
 *
 * The returned position is guaranteed to stay inside the shop room as far
 * as the map allows: the flight box is measured from the merchant tile up
 * to the nearest blocking tile (rock/wall/door/water/lava) in the four
 * cardinal directions, shrunk by a wall margin. If the map around the
 * merchant is not loaded yet (VOXEL_ROCK everywhere), a conservative
 * default box around the merchant is used until the chunk arrives.
 */
void tome_anim_update(int slot, int entity_id, int floor_id,
                      int mx, int my,
                      const TileType *map,
                      float dt,
                      float out_pos[3], float out_orient[16]);

#endif /* CLIENT_TOME_H */
