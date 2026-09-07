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

/*Number of frames that may be in flight (recorded but not yet presented).
 * The shared vertex buffer and the single command buffer are recycled
 * every MAX_FRAMES_IN_FLIGHT frames; each slot's fence guarantees the GPU
 * is done with them before the CPU writes again (see draw_frame).*/
#define MAX_FRAMES_IN_FLIGHT 3

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
    /*Three frames in flight (standard Vulkan triple-buffering): the CPU
     * records frame N+2 while the GPU runs N and presents N+1. Before
     * the fix a single fence plus vkWaitForFences(UINT64_MAX) at the end
     * of every frame blocked the CPU on the frame it had just submitted
     * — CPU and GPU never overlapped.*/
    VkSemaphore sem_image[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore sem_render[MAX_FRAMES_IN_FLIGHT];
    VkFence fences[MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame;
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
