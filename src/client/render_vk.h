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

/* Number of frames that may be in flight. Each frame owns its command
 * buffer and vertex buffer, so the CPU can safely prepare the next frame
 * while the GPU is still consuming previous frames. */
#define MAX_FRAMES_IN_FLIGHT 3

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float pos[3];
    float color[4];
    float normal[3];
} VkVertex;

/* ------------------------------------------------------------------ */
/* GPU-DRIVEN PATH (GPD): the CPU uploads compact INSTANCE descriptors */
/* into per-frame SSBOs; a compute pass culls them by vision radius and */
/* expands the primitive (box/pyramid) into a device-local vertex SSBO; */
/* a final 1-workgroup dispatch writes the VkDrawIndirectCommand that   */
/* the scene subpass consumes. The vertex shader reads the generated    */
/* vertices from storage (no fixed-function vertex input).              */
/*                                                                      */
/* Toggle: DRAGONGL_GPD=0 forces the legacy CPU-vertex path (kept as a  */
/* fallback for devices whose graphics queue lacks COMPUTE).            */
/* ------------------------------------------------------------------ */

/* MAP_INST_MAX: every floor tile becomes one instance; VOXEL_DOOR emits
 * TWO (door slab + threshold slab) -> 2 * 300 * 300 covers the worst
 * case. 2513 = CLIENT_MAX_ENTITIES(512) + player + MAX_PARTICLES(2000).
 * Capacity: worst visible window = (2*55+1)^2 tiles, each up to 72
 * vertices (double door), + dynamic worst case -> 880000 keeps slack. */
#define VKD_MAP_INST_MAX     (2u * 300u * 300u)
#define VKD_DYN_INST_MAX     2513u
#define VKD_ORIENTED_MAX     8u
#define VKD_VERTEX_CAPACITY  880000u
#define VKD_HUD_VERTS        100000u
#define VKD_CULL_RADIUS_MAX  55.0f
#define VKD_WORKGROUP        256u
/* GLSL GpdVertex stride (vec3 pos; vec4 color; vec3 normal -> 48 B).
 * Must match the struct in shaders/gpd_mesh.glsl. The CPU never writes
 * the vertex SSBO — this constant only sizes it. */
#define VKD_VERTEX_STRIDE    48u

/* Instance descriptor (48 B == vec4 x3, layout matches GpdInstance in
 * shaders/gpd_mesh.glsl — keep them in sync). a.w = type: 0=box, 1=pyramid,
 * 2=particle (box, never culled). */
typedef struct {
    float pos[3];   /* world center (tiles) */
    float type;     /* 0=box 1=pyramid 2=particle */
    float scale[3]; /* half extents */
    float pad;
    float color[3]; /* premultiplied for particles (mimics old additive) */
    float pad2;
} GpdInstance;

/* Rotated box (the floating tome of the book merchant; 64 B == vec4 x4,
 * matches GpdOriented). orient = column-major mat3, same convention as
 * push_box_oriented: world = pos + orient * (corner * scale). */
typedef struct {
    float pos[3];
    float type;
    float scale[3];
    float pad;
    float color[3];
    float pad2;
    float orient[9];
    float pad3[3];
} GpdOriented;

/* Compute push constants (24 B == GpdPC in shaders/gpd_mesh.glsl).
 * cull = (radius, playerX, playerZ, time). */
typedef struct {
    float cull_radius;
    float player_x;
    float player_z;
    float time;
    uint32_t count;     /* instances in the dispatch's input buffer */
    uint32_t capacity;  /* max vertices in the frame vertex SSBO */
} GpdPush;

/* GPU-side per-frame counters (16 B buffer == the GLSL `Counts` block
 * in shaders/gpd_mesh.glsl — keep in sync). The CPU zeroes the whole
 * buffer once per frame, right after the per-slot fence wait. The
 * cull/expand passes and the final indirect writer accumulate into it
 * on the GPU; with DRAGONGL_GPD_DEBUG the render thread also reads it
 * back (previous use of the slot, fence-waited) to cross-check cull vs
 * expand stage by stage. */
typedef struct {
    uint32_t verts;   /* total generated vertices (expand passes -> final) */
    uint32_t map_vis; /* visible map instances (cull_map -> expand_map) */
    uint32_t dyn_vis; /* visible dynamic instances (cull_dyn -> expand_dyn) */
} GpdCounts;

/*
 * Per-frame Vulkan resources.
 *
 * Command buffers and vertex buffers must not be reused while the GPU may
 * still be executing commands that reference them. Each frame-in-flight
 * therefore owns an independent command buffer and vertex buffer.
 */
typedef struct {
    VkCommandBuffer command_buffer;
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    void *mapped_vertex_data;

    /* --- GPU-driven per-frame ring (same triple-buffered invariant as
     * the vertex buffer above: the slot is fence-waited at the top of
     * draw_frame before any CPU write). ---
     * gpd_map_inst : full-floor tile instances, rewritten only when the
     *                slot's map generation is stale (map change/floor).
     * gpd_dyn_inst : entities + player + particles, rewritten each frame.
     * gpd_oriented : rotated boxes (tome), rewritten each frame.
     * gpd_verts    : GPU-generated vertices (device-local SSBO).
     * gpd_indirect : the VkDrawIndirectCommand written by the GPU.
     * gpd_counter  : atomic vertex counter (CPU resets it to 0, the
     *                compute dispatches accumulate into it). */
    /* gpd_map_inst / gpd_dyn_inst / gpd_oriented : instance lists the
     * CPU fills (host visible, persistent mapping).
     * gpd_map_visible / gpd_dyn_visible : GPU-culled index lists
     * (device local, GPU-only).
     * gpd_verts : GPU-generated vertices (device local).
     * gpd_indirect : the VkDrawIndirectCommand written by the GPU.
     * gpd_counts : the 3-GpdCounts buffer (host visible: the CPU resets
     * it, the GPU accumulates into it). */
    VkBuffer gpd_map_inst; VkDeviceMemory gpd_map_mem; void *gpd_map_ptr;
    VkBuffer gpd_dyn_inst; VkDeviceMemory gpd_dyn_mem; void *gpd_dyn_ptr;
    VkBuffer gpd_oriented; VkDeviceMemory gpd_ori_mem; void *gpd_ori_ptr;
    VkBuffer gpd_map_visible; VkDeviceMemory gpd_mvis_mem;
    VkBuffer gpd_dyn_visible; VkDeviceMemory gpd_dvis_mem;
    VkBuffer gpd_verts;    VkDeviceMemory gpd_vert_mem;
    VkBuffer gpd_indirect; VkDeviceMemory gpd_ind_mem; void *gpd_ind_ptr;
    VkBuffer gpd_counts;   VkDeviceMemory gpd_cnt_mem; void *gpd_cnt_ptr;
    /* One descriptor set per compute/scene pipeline use (7 total),
     * written once at creation — the buffers are stable per slot. */
    VkDescriptorSet gpd_desc_cull_map;
    VkDescriptorSet gpd_desc_cull_dyn;
    VkDescriptorSet gpd_desc_exp_map;
    VkDescriptorSet gpd_desc_exp_dyn;
    VkDescriptorSet gpd_desc_exp_ori;
    VkDescriptorSet gpd_desc_final;
    VkDescriptorSet gpd_desc_scene;
    uint32_t gpd_map_count;   /* valid instances in gpd_map_inst */
    uint32_t gpd_dyn_count;
    uint32_t gpd_ori_count;
    uint32_t gpd_slot_map_gen;/* generation of the map in gpd_map_inst */
} VkFrameResources;

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
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_hud;

    VkCommandPool command_pool;

    /* --- GPU-driven path (created once in vk_init, except the scene
     * pipeline which is size-dependent and follows the swapchain). --- */
    bool gpu_driven;
    /*
     * Culling and mesh allocation are SEPARATE compute passes (and are
     * developed/tested separately):
     *   cull_map / cull_dyn  : instances -> compact index lists (no
     *                         vertex SSBO access; verifiable in isolation)
     *   expand_map / expand_dyn / expand_oriented : index lists ->
     *                         vertex SSBO (capacity-guarded allocation)
     *   final                : counter -> VkDrawIndirectCommand
     * Each has its own set layout (different binding shapes), pipeline
     * layout (set 0 + 24 B compute push constants) and pipeline.
     */
    VkDescriptorSetLayout gpd_ds_layout_cull;     /* {0 in, 1 vis, 2 cnt} */
    VkDescriptorSetLayout gpd_ds_layout_exp;      /* {0 in, 1 vis, 2 cnt, 3 verts} */
    VkDescriptorSetLayout gpd_ds_layout_exp_ori;  /* {0 in, 2 cnt, 3 verts} */
    VkDescriptorSetLayout gpd_ds_layout_final;    /* {0 indirect, 1 cnt} */
    VkPipelineLayout gpd_pl_cull;
    VkPipelineLayout gpd_pl_exp;
    VkPipelineLayout gpd_pl_exp_ori;
    VkPipelineLayout gpd_pl_final;
    VkPipelineLayout gpd_scene_layout;            /* PC 80B + set 0 (1 binding) */
    VkDescriptorSetLayout gpd_scene_ds_layout;    /* 1 storage binding */
    VkDescriptorPool gpd_desc_pool;
    VkPipeline gpd_pipe_cull_map;
    VkPipeline gpd_pipe_cull_dyn;
    VkPipeline gpd_pipe_exp_map;
    VkPipeline gpd_pipe_exp_dyn;
    VkPipeline gpd_pipe_exp_ori;
    VkPipeline gpd_pipe_final;
    VkPipeline gpd_scene_pipeline;
    /* Global map generation: bumped by the render thread every time it
     * observes g_map_dirty; a per-slot gpd_slot_map_gen equal to it means
     * that slot's instance buffer is current. */
    uint32_t gpd_map_generation;

    /*
     * Independent resources for each frame in flight.
     */
    VkFrameResources frames[MAX_FRAMES_IN_FLIGHT];

    /*
     * Synchronization objects associated with each frame slot.
     */
    VkSemaphore sem_image[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore sem_render[MAX_FRAMES_IN_FLIGHT];
    VkFence fences[MAX_FRAMES_IN_FLIGHT];

    uint32_t current_frame;

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
