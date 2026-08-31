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

#ifndef RENDER_VK_H
#define RENDER_VK_H

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>

typedef struct {
    float pos[3];
    float color[4];
    float normal[3];
} VkVertex;

typedef struct {
    GLFWwindow *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t graphics_family;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *image_views;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_image_view;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_hud;
    VkFramebuffer *framebuffers;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore sem_image;
    VkSemaphore sem_render;
    VkFence fence_flight;
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    uint32_t vertex_count;
    uint32_t hud_vertex_count;
    uint32_t max_vertices;
    uint32_t current_image;
} VkState;

bool vk_init(VkState *s);
bool vk_recreate_swapchain(VkState *s, uint32_t width, uint32_t height);
void vk_cleanup(VkState *s);
void render_vk_start(void);

#endif // RENDER_VK_H
