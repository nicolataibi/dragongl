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
 * vk_internal.h - What the files of the Vulkan back end share with each
 * other (render_vk.h is what the rest of the client sees).
 */

#ifndef VK_INTERNAL_H
#define VK_INTERNAL_H

#include "render_vk.h"

/* vk_init.c: memory, buffers, shader modules */
bool vk_create_buffer(VkState *s, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                      VkBuffer *buf, VkDeviceMemory *mem, void **mapped);
VkShaderModule vk_load_shader(VkState *s, const char *name);
uint32_t vk_find_memory_type(VkState *s, uint32_t filter, VkMemoryPropertyFlags props);

/* vk_device.c */
bool vk_create_device(VkState *s);
bool vk_create_instance(VkState *s);
bool vk_pick_device(VkState *s);

/* vk_swapchain.c */
bool choose_depth_format(VkState *s);
bool vk_create_swapchain(VkState *s, uint32_t width, uint32_t height);
void vk_destroy_swapchain_resources(VkState *s);

/* vk_pipeline.c */
bool vk_create_pipelines(VkState *s);
void vk_destroy_pipelines(VkState *s);

/* vk_gpd.c */
bool vk_create_gpd_frame_buffers(VkState *s, uint32_t i);
bool vk_create_gpd_init(VkState *s);
bool vk_create_gpd_scene_pipeline(VkState *s);

#endif /* VK_INTERNAL_H */
