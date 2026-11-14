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
 * vk_swapchain.c - surface format, present mode, depth buffer and swapchain
 */

#include "render_vk.h"
#include "vk_internal.h"
#include "dragongl_config.h"
#include "client_state.h"
#include "client_particles.h"
#include "client_minimap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*Surface format: 8-bit sRGB (the colours of the game are tuned for it),
 *else whatever the surface lists first.*/
static VkSurfaceFormatKHR choose_surface_format(VkState *s) {
    VkSurfaceFormatKHR chosen = {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface, &n, NULL);
    VkSurfaceFormatKHR *f = n ? malloc(sizeof(*f) * n) : NULL;
    if (!f) {
        return chosen;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface, &n, f);
    const VkFormat prefer[2] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    bool found = false;
    for (int p = 0; p < 2 && !found; p++) {
        for (uint32_t i = 0; i < n; i++) {
            if (f[i].format == prefer[p] && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f[i];
                found = true;
                break;
            }
        }
    }
    if (!found && n > 0 && f[0].format != VK_FORMAT_UNDEFINED) {
        chosen = f[0];
    }
    free(f);
    return chosen;
}

/*FIFO (vsync) is always available. DRAGONGL_VSYNC=0 asks for MAILBOX,
 *then IMMEDIATE, if the surface offers them.*/
static VkPresentModeKHR choose_present_mode(VkState *s) {
    const char *vs = getenv("DRAGONGL_VSYNC");
    if (!vs || vs[0] != '0') {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface, &n, NULL);
    VkPresentModeKHR *m = n ? malloc(sizeof(*m) * n) : NULL;
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    if (m) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface, &n, m);
        for (uint32_t i = 0; i < n; i++) {
            if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                chosen = m[i];
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                chosen = m[i];
            }
        }
        free(m);
    }
    return chosen;
}

/*The first depth format the device can render to.*/
bool choose_depth_format(VkState *s) {
    const VkFormat candidates[4] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (int i = 0; i < 4; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s->physical_device, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s->depth_format = candidates[i];
            s->depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (candidates[i] == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                candidates[i] == VK_FORMAT_D24_UNORM_S8_UINT) {
                s->depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            return true;
        }
    }
    return false;
}

static bool vk_create_depth(VkState *s) {
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent.width = s->swapchain_extent.width;
    ci.extent.height = s->swapchain_extent.height;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = s->depth_format;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(s->device, &ci, NULL, &s->depth_image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s->device, s->depth_image, &req);
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        vk_find_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s->device, &ai, NULL, &s->depth_memory) != VK_SUCCESS ||
        vkBindImageMemory(s->device, s->depth_image, s->depth_memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s->depth_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s->depth_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(s->device, &vi, NULL, &s->depth_image_view) == VK_SUCCESS;
}

/*Everything that belongs to one swapchain: its image views, one
 *render-finished semaphore per image, the depth image. The swapchain
 *handle itself is destroyed separately (it may be the oldSwapchain of
 *the next one).*/
void vk_destroy_swapchain_resources(VkState *s) {
    if (s->image_views) {
        for (uint32_t i = 0; i < s->image_count; i++) {
            if (s->image_views[i]) {
                vkDestroyImageView(s->device, s->image_views[i], NULL);
            }
        }
        free((void *)s->image_views);
        s->image_views = NULL;
    }
    if (s->sem_render) {
        for (uint32_t i = 0; i < s->image_count; i++) {
            if (s->sem_render[i]) {
                vkDestroySemaphore(s->device, s->sem_render[i], NULL);
            }
        }
        free((void *)s->sem_render);
        s->sem_render = NULL;
    }
    free((void *)s->images);
    s->images = NULL;
    s->image_count = 0;
    if (s->depth_image_view) {
        vkDestroyImageView(s->device, s->depth_image_view, NULL);
        s->depth_image_view = VK_NULL_HANDLE;
    }
    if (s->depth_image) {
        vkDestroyImage(s->device, s->depth_image, NULL);
        s->depth_image = VK_NULL_HANDLE;
    }
    if (s->depth_memory) {
        vkFreeMemory(s->device, s->depth_memory, NULL);
        s->depth_memory = VK_NULL_HANDLE;
    }
}

/*(Re)create the swapchain for a framebuffer of width x height pixels.
 *The previous swapchain, if any, is handed over as oldSwapchain and then
 *destroyed. The extent follows the surface when it imposes one.*/
bool vk_create_swapchain(VkState *s, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical_device, s->surface, &caps) !=
        VK_SUCCESS) {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width;
        extent.height = height;
        if (extent.width < caps.minImageExtent.width) {
            extent.width = caps.minImageExtent.width;
        }
        if (extent.height < caps.minImageExtent.height) {
            extent.height = caps.minImageExtent.height;
        }
        if (extent.width > caps.maxImageExtent.width) {
            extent.width = caps.maxImageExtent.width;
        }
        if (extent.height > caps.maxImageExtent.height) {
            extent.height = caps.maxImageExtent.height;
        }
    }
    if (extent.width == 0 || extent.height == 0) {
        return false; /* minimized: the caller waits */
    }

    VkSurfaceFormatKHR fmt = choose_surface_format(s);
    uint32_t min_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && min_images > caps.maxImageCount) {
        min_images = caps.maxImageCount;
    }

    VkSwapchainKHR old = s->swapchain;
    VkSwapchainCreateInfoKHR sc_ci = {0};
    sc_ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_ci.surface = s->surface;
    sc_ci.minImageCount = min_images;
    sc_ci.imageFormat = fmt.format;
    sc_ci.imageColorSpace = fmt.colorSpace;
    sc_ci.imageExtent = extent;
    sc_ci.imageArrayLayers = 1;
    sc_ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_ci.preTransform = caps.currentTransform;
    sc_ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                               ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                               : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sc_ci.presentMode = choose_present_mode(s);
    sc_ci.clipped = VK_TRUE;
    sc_ci.oldSwapchain = old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(s->device, &sc_ci, NULL, &created);
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(s->device, old, NULL);
    }
    s->swapchain = created;
    if (r != VK_SUCCESS) {
        s->swapchain = VK_NULL_HANDLE;
        printf("[VK] Swapchain creation error (%d)\n", r);
        return false;
    }
    s->swapchain_format = fmt.format;
    s->swapchain_extent = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &count, NULL);
    s->images = (VkImage *)calloc(count, sizeof(VkImage));
    s->image_views = (VkImageView *)calloc(count, sizeof(VkImageView));
    s->sem_render = (VkSemaphore *)calloc(count, sizeof(VkSemaphore));
    if (!s->images || !s->image_views || !s->sem_render) {
        return false;
    }
    s->image_count = count;
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &count, s->images);
    VkSemaphoreCreateInfo sem_ci = {0};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo iv_ci = {0};
        iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_ci.image = s->images[i];
        iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_ci.format = s->swapchain_format;
        iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_ci.subresourceRange.levelCount = 1;
        iv_ci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s->device, &iv_ci, NULL, &s->image_views[i]) != VK_SUCCESS ||
            vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_render[i]) != VK_SUCCESS) {
            return false;
        }
    }
    if (!vk_create_depth(s)) {
        printf("[VK] Depth buffer creation error\n");
        return false;
    }
    s->current_image = 0;
    return true;
}

/*Rebuild the swapchain and its resources (window resized, OUT_OF_DATE,
 *SUBOPTIMAL). The pipelines are rebuilt only if the colour format
 *changed (they do not depend on the size).*/
bool vk_recreate_swapchain(VkState *s, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return true;
    }
    vkDeviceWaitIdle(s->device);
    vk_destroy_swapchain_resources(s);
    if (!vk_create_swapchain(s, width, height)) {
        printf("[VK] Swapchain recreation error %ux%u\n", width, height);
        return false;
    }
    if (s->swapchain_format != s->pipelines_format) {
        vk_destroy_pipelines(s);
        if (!vk_create_pipelines(s)) {
            return false;
        }
    }
    return true;
}

/* --- Instance and device -------------------------------------------------- */
