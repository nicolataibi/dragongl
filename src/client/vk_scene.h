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
 * vk_scene.h - The instances of the 3D scene: the tiles of the map around
 * the player, the entities and the particles, written every frame into
 * the buffers of the frame slot (the GPU culls and expands them).
 */

#ifndef VK_SCENE_H
#define VK_SCENE_H

#include "client_state.h"
#include "render_vk.h"

/*Fills the instance buffers of the current frame slot from the snapshot.
 * dt: seconds since the previous frame (entities glide to their tile).
 * particles: false leaves the particle effects out (reduce_motion).*/
void vk_scene_update(VkState *s, float dt, const FrameSnapshot *snap, bool particles);
/*Where entity slot i is drawn (its position while it glides); false when
 * the slot has not been drawn yet.*/
bool vk_scene_entity_pos(int slot, float *x, float *z);
/*Forgets the map window and frees its cache (shutdown, renderer
 * restart).*/
void vk_scene_reset(void);

#endif /* VK_SCENE_H */
