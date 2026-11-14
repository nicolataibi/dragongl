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

/*Index of a memory type allowed by `filter` with all of `props`, or
 *UINT32_MAX if the device has none.*/
uint32_t vk_find_memory_type(VkState *s, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(s->physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

/*Create a buffer with its own memory: host visible and coherent (and
 *persistently mapped into *mapped) when `host`, device local otherwise.*/
bool vk_create_buffer(VkState *s, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                      VkBuffer *buf, VkDeviceMemory *mem, void **mapped) {
    VkBufferCreateInfo bc = {0};
    bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bc.size = size;
    bc.usage = usage;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s->device, &bc, NULL, buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s->device, *buf, &req);
    uint32_t type = vk_find_memory_type(
        s, req.memoryTypeBits,
        host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(s->device, &ai, NULL, mem) != VK_SUCCESS) {
        return false;
    }
    if (vkBindBufferMemory(s->device, *buf, *mem, 0) != VK_SUCCESS) {
        return false;
    }
    if (host && mapped && vkMapMemory(s->device, *mem, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        return false;
    }
    return true;
}

/*Read a SPIR-V file of shaders/ (name like "shader.frag.spv"). Searched:
 *  1. $DRAGONGL_SHADER_DIR, if set;
 *  2. <directory of the executable>/shaders (build tree: build/shaders);
 *  3. DRAGONGL_PKGDATADIR/shaders (installed: /usr/share/dragongl/shaders).
 *The current directory is not searched: what the client runs must not
 *depend on where it was started from.*/
VkShaderModule vk_load_shader(VkState *s, const char *name) {
    char dirs[3][4096];
    int n_dirs = 0;
    const char *env = getenv("DRAGONGL_SHADER_DIR");
    if (env && env[0]) {
        snprintf(dirs[n_dirs++], sizeof(dirs[0]), "%s", env);
    }
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(dirs[n_dirs++], sizeof(dirs[0]), "%.4000s/shaders", exe);
        }
    }
    snprintf(dirs[n_dirs++], sizeof(dirs[0]), "%s/shaders", DRAGONGL_PKGDATADIR);

    FILE *f = NULL;
    char path[sizeof(dirs[0]) + 64];
    for (int d = 0; d < n_dirs && !f; d++) {
        if (snprintf(path, sizeof(path), "%.4095s/%.60s", dirs[d], name) < (int)sizeof(path)) {
            f = fopen(path, "rb");
        }
    }
    if (!f) {
        printf(
            "[VK] Shader %s not found (DRAGONGL_SHADER_DIR, next to the executable, " DRAGONGL_PKGDATADIR
            "/shaders).\n",
            name);
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = VK_NULL_HANDLE;
    long size = -1;
    uint32_t *code = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size > 0 && size % 4 == 0 && fseek(f, 0, SEEK_SET) == 0) {
        code = malloc((size_t)size);
    }
    if (code && fread(code, 1, (size_t)size, f) == (size_t)size) {
        VkShaderModuleCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = (size_t)size;
        ci.pCode = code;
        if (vkCreateShaderModule(s->device, &ci, NULL, &mod) != VK_SUCCESS) {
            mod = VK_NULL_HANDLE;
        }
    }
    if (mod == VK_NULL_HANDLE) {
        printf("[VK] Invalid shader %s\n", path);
    }
    free(code);
    fclose(f);
    return mod;
}

bool vk_init(VkState *s) {
    if (!vk_create_instance(s)) {
        return false;
    }
    if (glfwCreateWindowSurface(s->instance, s->window, NULL, &s->surface) != VK_SUCCESS) {
        printf("[VK] Error creating the window surface\n");
        return false;
    }
    if (!vk_pick_device(s) || !vk_create_device(s)) {
        return false;
    }
    if (!choose_depth_format(s)) {
        printf("[VK] No depth format usable as an attachment\n");
        return false;
    }

    /*HUD pipeline layout: mat4 + 4 floats of push constants (80 bytes,
     *the guaranteed minimum is 128).*/
    VkPushConstantRange push_const = {0};
    push_const.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_const.size = sizeof(float) * 20;
    VkPipelineLayoutCreateInfo pl_ci = {0};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_const;
    if (vkCreatePipelineLayout(s->device, &pl_ci, NULL, &s->pipeline_layout) != VK_SUCCESS) {
        return false;
    }

    /*GPU-driven scene: descriptor layouts, compute pipelines, pool.*/
    if (!vk_create_gpd_init(s)) {
        printf("[VK] Compute pipeline initialization failed\n");
        return false;
    }

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(s->window, &fb_w, &fb_h);
    if (!vk_create_swapchain(s, (uint32_t)(fb_w > 0 ? fb_w : 1), (uint32_t)(fb_h > 0 ? fb_h : 1)) ||
        !vk_create_pipelines(s)) {
        return false;
    }

    VkCommandPoolCreateInfo cp_ci = {0};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = s->graphics_family;
    if (vkCreateCommandPool(s->device, &cp_ci, NULL, &s->command_pool) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo cb_ai = {0};
    cb_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_ai.commandPool = s->command_pool;
    cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
    if (vkAllocateCommandBuffers(s->device, &cb_ai, command_buffers) != VK_SUCCESS) {
        printf("[VK] Command buffer allocation error\n");
        return false;
    }

    /*Per frame slot: an image-available semaphore and a fence that
     *starts signalled (the first wait of each slot returns at once).
     *The render-finished semaphores belong to the swapchain images.*/
    VkSemaphoreCreateInfo sem_ci = {0};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fen_ci = {0};
    fen_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fen_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    s->current_frame = 0;
    s->max_vertices = VKD_HUD_VERTS;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkFrameResources *frame = &s->frames[i];
        frame->command_buffer = command_buffers[i];
        if (vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_image[i]) != VK_SUCCESS ||
            vkCreateFence(s->device, &fen_ci, NULL, &s->fences[i]) != VK_SUCCESS) {
            return false;
        }
        /*The slot's 2D HUD vertices (the 3D scene is generated on the
         *GPU into the device-local gpd_verts).*/
        if (!vk_create_buffer(s, sizeof(VkVertex) * (VkDeviceSize)s->max_vertices,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, &frame->vertex_buffer,
                              &frame->vertex_memory, &frame->mapped_vertex_data)) {
            printf("[VK] HUD vertex buffer creation error (frame %u)\n", i);
            return false;
        }
        if (!vk_create_gpd_frame_buffers(s, i)) {
            printf("[VK] Scene buffer creation error (frame %u)\n", i);
            return false;
        }
    }

    printf("[VK] Vulkan initialization complete (%ux%u).\n", s->swapchain_extent.width,
           s->swapchain_extent.height);
    return true;
}

/*Safe after a partial vk_init(): every handle is checked.*/
void vk_cleanup(VkState *s) {
    if (s->device) {
        vkDeviceWaitIdle(s->device);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkFrameResources *f = &s->frames[i];
            VkBuffer bufs[] = {f->vertex_buffer, f->gpd_map_inst,    f->gpd_dyn_inst,
                               f->gpd_oriented,  f->gpd_map_visible, f->gpd_dyn_visible,
                               f->gpd_verts,     f->gpd_indirect,    f->gpd_counts};
            VkDeviceMemory mems[] = {f->vertex_memory, f->gpd_map_mem,  f->gpd_dyn_mem,
                                     f->gpd_ori_mem,   f->gpd_mvis_mem, f->gpd_dvis_mem,
                                     f->gpd_vert_mem,  f->gpd_ind_mem,  f->gpd_cnt_mem};
            for (size_t k = 0; k < sizeof(bufs) / sizeof(bufs[0]); k++) {
                if (bufs[k]) {
                    vkDestroyBuffer(s->device, bufs[k], NULL);
                }
            }
            /* freeing mapped memory unmaps it */
            for (size_t k = 0; k < sizeof(mems) / sizeof(mems[0]); k++) {
                if (mems[k]) {
                    vkFreeMemory(s->device, mems[k], NULL);
                }
            }
            memset(f, 0, sizeof(*f));
            if (s->fences[i]) {
                vkDestroyFence(s->device, s->fences[i], NULL);
            }
            if (s->sem_image[i]) {
                vkDestroySemaphore(s->device, s->sem_image[i], NULL);
            }
            s->fences[i] = VK_NULL_HANDLE;
            s->sem_image[i] = VK_NULL_HANDLE;
        }
        if (s->command_pool) {
            vkDestroyCommandPool(s->device, s->command_pool, NULL);
        }

        /*Compute resources (the descriptor sets die with the pool).*/
        if (s->gpd_desc_pool) {
            vkDestroyDescriptorPool(s->device, s->gpd_desc_pool, NULL);
        }
        VkDescriptorSetLayout dsl[] = {s->gpd_ds_layout_cull, s->gpd_ds_layout_exp,
                                       s->gpd_ds_layout_exp_ori, s->gpd_ds_layout_final,
                                       s->gpd_scene_ds_layout};
        for (size_t k = 0; k < sizeof(dsl) / sizeof(dsl[0]); k++) {
            if (dsl[k]) {
                vkDestroyDescriptorSetLayout(s->device, dsl[k], NULL);
            }
        }
        VkPipeline pipes[] = {s->gpd_pipe_cull_map, s->gpd_pipe_cull_dyn, s->gpd_pipe_exp_map,
                              s->gpd_pipe_exp_dyn,  s->gpd_pipe_exp_ori,  s->gpd_pipe_final};
        for (size_t k = 0; k < sizeof(pipes) / sizeof(pipes[0]); k++) {
            if (pipes[k]) {
                vkDestroyPipeline(s->device, pipes[k], NULL);
            }
        }
        VkPipelineLayout pls[] = {s->gpd_pl_cull,  s->gpd_pl_exp,       s->gpd_pl_exp_ori,
                                  s->gpd_pl_final, s->gpd_scene_layout, s->pipeline_layout};
        for (size_t k = 0; k < sizeof(pls) / sizeof(pls[0]); k++) {
            if (pls[k]) {
                vkDestroyPipelineLayout(s->device, pls[k], NULL);
            }
        }
        vk_destroy_pipelines(s);
        vk_destroy_swapchain_resources(s);
        if (s->swapchain) {
            vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        }
        vkDestroyDevice(s->device, NULL);
    }
    if (s->surface) {
        vkDestroySurfaceKHR(s->instance, s->surface, NULL);
    }
    if (s->instance) {
        vkDestroyInstance(s->instance, NULL);
    }
    GLFWwindow *window = s->window;
    memset(s, 0, sizeof(*s));
    s->window = window;
}
