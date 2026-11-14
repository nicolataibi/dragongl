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
 * vk_frame.h - One frame of the Vulkan renderer.
 */

#ifndef VK_FRAME_H
#define VK_FRAME_H

#include "render_vk.h"

/*Draws and presents one frame. dt: seconds since the previous one; time:
 * seconds since the start (animations of the shaders); frame_ms: the
 * average frame time the HUD may show.
 *
 * On return s->swapchain_dirty asks for a new swapchain before the next
 * frame (resize, OUT_OF_DATE, SUBOPTIMAL) and s->device_lost says that
 * the device or the surface is gone: the caller restarts the renderer.*/
void vk_draw_frame(VkState *s, float dt, double time, float frame_ms);

#endif /* VK_FRAME_H */
