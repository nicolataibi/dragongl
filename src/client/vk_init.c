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
#ifndef VK_API_VERSION_1_4
#define VK_API_VERSION_1_4 VK_MAKE_API_VERSION(0, 1, 4, 0)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES 1000570000

typedef struct VkPhysicalDeviceVulkan14Features {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           globalPriorityQuery;
    VkBool32           shaderSubcluster;
    VkBool32           hostImageCopy;
    VkBool32           localRead;
    VkBool32           maintenance5;
    VkBool32           maintenance6;
    VkBool32           pipelineProtectedAccess;
    VkBool32           pipelineRobustness;
    VkBool32           hostQueryReset;
    VkBool32           ycbcr2Plane444Formats;
    VkBool32           ycbcrDegamma;
    VkBool32           transformFeedback;
    VkBool32           dynamicRenderingLocalRead;
    VkBool32           indexTypeUint8;
    VkBool32           lineRasterization;
    VkBool32           nodePayloads;
    VkBool32           pushDescriptor;
} VkPhysicalDeviceVulkan14Features;
#endif
#include "client_state.h"
#include "client_particles.h"
#include "client_minimap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t vk_find_memory_type(VkState *s, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s->physical_device, &mem_props);
    for (i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

/*Resolve a shader path in order:
 *  1. relative to CWD (development layout, the shaders directory)
 *  2. build/<path> (unpacked build tree)
 *  3. /usr/share/dragongl/<path> (RPM install: the client runs from
 *     /usr/bin where no local shaders/ exists — without this fallback
 *     the packaged client could never start the Vulkan backend).*/
static VkShaderModule vk_load_shader(VkState *s, const char *path) {
    FILE *f;
    long size;
    char *code;
    VkShaderModuleCreateInfo ci;
    VkShaderModule mod;
    f = fopen(path, "rb");
    if (!f) {
        char alt_path[256];
        snprintf(alt_path, sizeof(alt_path), "build/%s", path);
        f = fopen(alt_path, "rb");
    }
    if (!f) {
        char sys_path[256];
        snprintf(sys_path, sizeof(sys_path), "/usr/share/dragongl/%s", path);
        f = fopen(sys_path, "rb");
        if (!f) {
            printf("Shader opening error: '%s', 'build/%s' and '/usr/share/dragongl/%s' all failed.\n",
                   path, path, path);
            return VK_NULL_HANDLE;
        }
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    code = malloc(size);
    size_t _r2 = fread(code, 1, size, f); (void)_r2;
    fclose(f);
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = (const uint32_t*)code;
    vkCreateShaderModule(s->device, &ci, NULL, &mod);
    free(code);
    return mod;
}



/*=========================================================================
 * GPU-DRIVEN PATH (GPD) — resource creation.
 *
 * vk_create_gpd_init           : descriptor layouts, pipeline layouts,
 *                                descriptor pool, the 4 compute pipelines.
 *                                Size-independent (device lifetime).
 * vk_create_gpd_scene_pipeline : the scene pipeline that draws the
 *                                compute-generated vertices with
 *                                vkCmdDrawIndirect. Size-dependent
 *                                (render pass / swapchain lifetime), so it
 *                                is created from vk_create_pipelines.
 * vk_create_gpd_frame_buffers  : the per-slot ring of GPD buffers + the
 *                                five per-slot descriptor sets.
 *=========================================================================*/

/*Compute pipeline helper: one stage, the caller's pipeline layout.*/
static bool gpd_create_compute_pipeline(VkState *s, const char *spv_path,
                                        VkPipelineLayout layout, VkPipeline *out) {
    VkShaderModule mod = vk_load_shader(s, spv_path);
    if (mod == VK_NULL_HANDLE) return false;
    VkPipelineShaderStageCreateInfo stage = {0};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo cp_ci = {0};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage = stage;
    cp_ci.layout = layout;
    bool ok = (vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &cp_ci, NULL, out) == VK_SUCCESS);
    vkDestroyShaderModule(s->device, mod, NULL);
    return ok;
}

/*One storage-buffer binding (the only descriptor type the GPD path
 * ever uses).*/
static void gpd_binding(VkDescriptorSetLayoutBinding *b, uint32_t index) {
    b->binding = index;
    b->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b->descriptorCount = 1;
    b->stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
}

/*Persistent GPD resources (set layouts, pipeline layouts, pool, the 6
 * compute pipelines).*/
static bool vk_create_gpd_init(VkState *s) {
    VkDescriptorSetLayoutBinding bindings[4];
    VkDescriptorSetLayoutCreateInfo dsl_ci;
    VkPushConstantRange pc_range;
    VkPipelineLayoutCreateInfo pl_ci;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo pool_ci;
    int p;

    /*Set 0 layouts (one per binding shape — see the pipeline table in
     * vk_create_gpd_frame_buffers for which set fills which buffer).*/
    gpd_binding(&bindings[0], 0); gpd_binding(&bindings[1], 1); gpd_binding(&bindings[2], 2);
    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 3;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, &s->gpd_ds_layout_cull) != VK_SUCCESS) return false;

    gpd_binding(&bindings[3], 3);
    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 4;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, &s->gpd_ds_layout_exp) != VK_SUCCESS) return false;

    /*expand_oriented skips binding 1 (no index list): {0, 2, 3} —
     * binding numbers need not be contiguous.*/
    gpd_binding(&bindings[0], 0); gpd_binding(&bindings[1], 2); gpd_binding(&bindings[2], 3);
    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 3;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, &s->gpd_ds_layout_exp_ori) != VK_SUCCESS) return false;

    gpd_binding(&bindings[0], 0); gpd_binding(&bindings[1], 1);
    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 2;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, &s->gpd_ds_layout_final) != VK_SUCCESS) return false;

    /*Set 0 (scene): the compute-generated vertex SSBO.*/
    memset(&bindings[0], 0, sizeof(bindings[0]));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, &s->gpd_scene_ds_layout) != VK_SUCCESS) return false;

    /*Pipeline layouts: each compute set layout + the 24 B compute push
     * constants (GpdPush == GpdPC in gpd_mesh.glsl).*/
    memset(&pc_range, 0, sizeof(pc_range));
    pc_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc_range.offset = 0;
    pc_range.size = sizeof(GpdPush);
    {
        VkDescriptorSetLayout set_layouts[4] = {
            s->gpd_ds_layout_cull, s->gpd_ds_layout_exp,
            s->gpd_ds_layout_exp_ori, s->gpd_ds_layout_final
        };
        VkPipelineLayout *pls[4] = {
            &s->gpd_pl_cull, &s->gpd_pl_exp, &s->gpd_pl_exp_ori, &s->gpd_pl_final
        };
        for (p = 0; p < 4; p++) {
            memset(&pl_ci, 0, sizeof(pl_ci));
            pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl_ci.setLayoutCount = 1;
            pl_ci.pSetLayouts = &set_layouts[p];
            pl_ci.pushConstantRangeCount = 1;
            pl_ci.pPushConstantRanges = &pc_range;
            if (vkCreatePipelineLayout(s->device, &pl_ci, NULL, pls[p]) != VK_SUCCESS) return false;
        }
    }

    /*Same PushConstants as the legacy pipelines (mvp, visionRadius,
     * playerX, playerZ, time).*/
    memset(&pc_range, 0, sizeof(pc_range));
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc_range.offset = 0;
    pc_range.size = sizeof(float) * 20;
    memset(&pl_ci, 0, sizeof(pl_ci));
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &s->gpd_scene_ds_layout;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pc_range;
    if (vkCreatePipelineLayout(s->device, &pl_ci, NULL, &s->gpd_scene_layout) != VK_SUCCESS) return false;

    /*7 sets per in-flight frame (one per pipeline use; see
     * vk_create_gpd_frame_buffers). 3+3+4+4+3+2+1 = 20 descriptors.*/
    memset(&pool_size, 0, sizeof(pool_size));
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 20 * 4; // 20 descriptors per frame, up to 4 frames
    memset(&pool_ci, 0, sizeof(pool_ci));
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 7 * 4; // 7 sets per frame
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(s->device, &pool_ci, NULL, &s->gpd_desc_pool) != VK_SUCCESS) return false;

    /*Cull passes (stage 1) then expand passes (stage 2) then final.*/
    struct { const char *spv; VkPipelineLayout pl; VkPipeline *out; } pipes[6] = {
        { "shaders/gpd_cull_map.comp.spv",         s->gpd_pl_cull,    &s->gpd_pipe_cull_map },
        { "shaders/gpd_cull_dyn.comp.spv",         s->gpd_pl_cull,    &s->gpd_pipe_cull_dyn },
        { "shaders/gpd_expand_map.comp.spv",       s->gpd_pl_exp,     &s->gpd_pipe_exp_map },
        { "shaders/gpd_expand_dyn.comp.spv",       s->gpd_pl_exp,     &s->gpd_pipe_exp_dyn },
        { "shaders/gpd_expand_oriented.comp.spv",  s->gpd_pl_exp_ori, &s->gpd_pipe_exp_ori },
        { "shaders/gpd_final.comp.spv",            s->gpd_pl_final,   &s->gpd_pipe_final }
    };
    for (p = 0; p < 6; p++) {
        if (!gpd_create_compute_pipeline(s, pipes[p].spv, pipes[p].pl, pipes[p].out)) return false;
    }
    return true;
}

/*The GPD scene pipeline: rasterization/depth/viewport identical to the
 * legacy scene pipeline, EXCEPT no vertex input — the vertex shader reads
 * the compute-generated vertices from the storage buffer (binding 0).*/
static bool vk_create_gpd_scene_pipeline(VkState *s) {
    VkShaderModule vert_mod;
    VkShaderModule frag_mod;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi_ci;
    VkPipelineInputAssemblyStateCreateInfo ia_ci;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo vp_ci;
    VkPipelineRasterizationStateCreateInfo rs_ci;
    VkPipelineMultisampleStateCreateInfo ms_ci;
    VkPipelineDepthStencilStateCreateInfo ds_ci;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb_ci;
    VkGraphicsPipelineCreateInfo gp_ci;

    vert_mod = vk_load_shader(s, "shaders/gpd_scene.vert.spv");
    frag_mod = vk_load_shader(s, "shaders/shader.frag.spv");
    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) return false;
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    /*No vertex input at all: gl_VertexIndex indexes the SSBO directly.*/
    memset(&vi_ci, 0, sizeof(vi_ci));
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia_ci, 0, sizeof(ia_ci));
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&viewport, 0, sizeof(viewport));
    viewport.width = (float)s->swapchain_extent.width;
    viewport.height = (float)s->swapchain_extent.height;
    viewport.maxDepth = 1.0f;
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent = s->swapchain_extent;
    memset(&vp_ci, 0, sizeof(vp_ci));
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.pViewports = &viewport;
    vp_ci.scissorCount = 1;
    vp_ci.pScissors = &scissor;
    memset(&rs_ci, 0, sizeof(rs_ci));
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rs_ci.lineWidth = 1.0f;
    rs_ci.cullMode = VK_CULL_MODE_NONE;
    rs_ci.frontFace = VK_FRONT_FACE_CLOCKWISE;
    memset(&ms_ci, 0, sizeof(ms_ci));
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds_ci, 0, sizeof(ds_ci));
    ds_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds_ci.depthTestEnable = VK_TRUE;
    ds_ci.depthWriteEnable = VK_TRUE;
    ds_ci.depthCompareOp = VK_COMPARE_OP_LESS;
    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb_ci, 0, sizeof(cb_ci));
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &cba;

    memset(&gp_ci, 0, sizeof(gp_ci));
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDepthStencilState = &ds_ci;
    VkPipelineRenderingCreateInfo pr_ci;
    memset(&pr_ci, 0, sizeof(pr_ci));
    pr_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr_ci.colorAttachmentCount = 1;
    pr_ci.pColorAttachmentFormats = &s->swapchain_format;
    pr_ci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    gp_ci.layout = s->gpd_scene_layout;
    gp_ci.pNext = &pr_ci;
    gp_ci.renderPass = VK_NULL_HANDLE;
    gp_ci.subpass = 0;
    bool ok = (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp_ci, NULL,
                                         &s->gpd_scene_pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(s->device, vert_mod, NULL);
    vkDestroyShaderModule(s->device, frag_mod, NULL);
    return ok;
}

/*Per-slot GPD ring: 8 buffers + the 7 per-slot descriptor sets
 * (written once — the buffers never change after creation).
 *
 *   buffer          filled by    read by
 *   gpd_map_inst    CPU (walk)   cull_map, expand_map
 *   gpd_dyn_inst    CPU (per fr) cull_dyn, expand_dyn
 *   gpd_oriented    CPU (per fr) expand_oriented
 *   gpd_map_visible GPU cull_map expand_map
 *   gpd_dyn_visible GPU cull_dyn expand_dyn
 *   gpd_verts       GPU expand   scene (drawIndirect)
 *   gpd_indirect    GPU final    vkCmdDrawIndirect
 *   gpd_counts      CPU reset +  cull/expand/final (GPU accumulate)      */
static bool vk_create_gpd_frame_buffers(VkState *s, uint32_t i) {
    VkFrameResources *f = &s->frames[i];
    VkBufferCreateInfo bc;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkDescriptorSetLayout layouts[7];
    VkDescriptorSet sets[7];
    VkDescriptorSetAllocateInfo sai;
    VkDescriptorBufferInfo bi[20] = {0};
    VkWriteDescriptorSet w[20] = {0}; /* dstArrayElement = 0 for all */
    VkDeviceSize sizes[8];
    VkBufferUsageFlags usages[8];
    VkBuffer *bufs[8];
    VkDeviceMemory *mems[8];
    void **ptrs[8];
    bool host[8];
    uint32_t d;
    int n, k;

    bufs[0] = &f->gpd_map_inst;    mems[0] = &f->gpd_map_mem;    ptrs[0] = &f->gpd_map_ptr;
    bufs[1] = &f->gpd_dyn_inst;    mems[1] = &f->gpd_dyn_mem;    ptrs[1] = &f->gpd_dyn_ptr;
    bufs[2] = &f->gpd_oriented;    mems[2] = &f->gpd_ori_mem;    ptrs[2] = &f->gpd_ori_ptr;
    bufs[3] = &f->gpd_map_visible; mems[3] = &f->gpd_mvis_mem;   ptrs[3] = NULL;
    bufs[4] = &f->gpd_dyn_visible; mems[4] = &f->gpd_dvis_mem;   ptrs[4] = NULL;
    bufs[5] = &f->gpd_verts;       mems[5] = &f->gpd_vert_mem;   ptrs[5] = NULL;
    bufs[6] = &f->gpd_indirect;    mems[6] = &f->gpd_ind_mem;    ptrs[6] = &f->gpd_ind_ptr;
    bufs[7] = &f->gpd_counts;      mems[7] = &f->gpd_cnt_mem;    ptrs[7] = &f->gpd_cnt_ptr;
    sizes[0] = (VkDeviceSize)VKD_MAP_INST_MAX * sizeof(GpdInstance);
    sizes[1] = (VkDeviceSize)VKD_DYN_INST_MAX * sizeof(GpdInstance);
    sizes[2] = (VkDeviceSize)VKD_ORIENTED_MAX * sizeof(GpdOriented);
    sizes[3] = (VkDeviceSize)VKD_MAP_INST_MAX * sizeof(uint32_t); /*cull indices*/
    sizes[4] = (VkDeviceSize)VKD_DYN_INST_MAX * sizeof(uint32_t); /*cull indices*/
    /*VKD_VERTEX_STRIDE must match the GLSL GpdVertex layout in
     * gpd_mesh.glsl (vec3 pos; vec4 color; vec3 normal -> 48 B).*/
    sizes[5] = (VkDeviceSize)VKD_VERTEX_CAPACITY * VKD_VERTEX_STRIDE;
    sizes[6] = 16; /*one VkDrawIndirectCommand*/
    sizes[7] = 16; /*GpdCounts: 3 x uint32, padded to 16*/
    usages[0] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[1] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[2] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[3] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[4] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[5] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    usages[6] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    usages[7] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    host[0] = true;  host[1] = true;  host[2] = true;
    host[3] = false; /*device-local: GPU-only (cull output)*/
    host[4] = false;
    host[5] = false; /*device-local: GPU writes it, the CPU never does*/
    host[6] = true;  host[7] = true;

    for (d = 0; d < 8; d++) {
        memset(&bc, 0, sizeof(bc));
        bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bc.size = sizes[d];
        bc.usage = usages[d];
        bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bc, NULL, bufs[d]) != VK_SUCCESS) return false;
        vkGetBufferMemoryRequirements(s->device, *bufs[d], &req);
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk_find_memory_type(s, req.memoryTypeBits,
            host[d] ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(s->device, &ai, NULL, mems[d]) != VK_SUCCESS) return false;
        if (vkBindBufferMemory(s->device, *bufs[d], *mems[d], 0) != VK_SUCCESS) return false;
        if (ptrs[d] &&
            vkMapMemory(s->device, *mems[d], 0, req.size, 0, ptrs[d]) != VK_SUCCESS) {
            return false;
        }
    }

    /*7 sets: one per pipeline use (the binding table above).*/
    layouts[0] = s->gpd_ds_layout_cull;      /*cull_map: 0=map_inst 1=m_visible 2=counts*/
    layouts[1] = s->gpd_ds_layout_cull;      /*cull_dyn: 0=dyn_inst 1=d_visible 2=counts*/
    layouts[2] = s->gpd_ds_layout_exp;       /*exp_map:  0=map_inst 1=m_visible 2=counts 3=verts*/
    layouts[3] = s->gpd_ds_layout_exp;       /*exp_dyn:  0=dyn_inst 1=d_visible 2=counts 3=verts*/
    layouts[4] = s->gpd_ds_layout_exp_ori;   /*exp_ori:  0=oriented       2=counts 3=verts*/
    layouts[5] = s->gpd_ds_layout_final;     /*final:    0=indirect       1=counts     */
    layouts[6] = s->gpd_scene_ds_layout;     /*scene:    0=verts                     */
    memset(&sai, 0, sizeof(sai));
    sai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sai.descriptorPool = s->gpd_desc_pool;
    sai.descriptorSetCount = 7;
    sai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &sai, sets) != VK_SUCCESS) return false;
    f->gpd_desc_cull_map = sets[0];
    f->gpd_desc_cull_dyn = sets[1];
    f->gpd_desc_exp_map  = sets[2];
    f->gpd_desc_exp_dyn  = sets[3];
    f->gpd_desc_exp_ori  = sets[4];
    f->gpd_desc_final    = sets[5];
    f->gpd_desc_scene    = sets[6];

    /*20 writes: 3+3 (cull) + 4+4 (expand) + 3 (exp_ori) + 2 (final) + 1
     * (scene). Table-driven: {set, binding, buffer}.*/
    struct { uint32_t set, binding; VkBuffer buf; } table[20] = {
        { 0, 0, f->gpd_map_inst },   { 0, 1, f->gpd_map_visible }, { 0, 2, f->gpd_counts },
        { 1, 0, f->gpd_dyn_inst },   { 1, 1, f->gpd_dyn_visible }, { 1, 2, f->gpd_counts },
        { 2, 0, f->gpd_map_inst },   { 2, 1, f->gpd_map_visible }, { 2, 2, f->gpd_counts }, { 2, 3, f->gpd_verts },
        { 3, 0, f->gpd_dyn_inst },   { 3, 1, f->gpd_dyn_visible }, { 3, 2, f->gpd_counts }, { 3, 3, f->gpd_verts },
        { 4, 0, f->gpd_oriented },   { 4, 2, f->gpd_counts },      { 4, 3, f->gpd_verts },
        { 5, 0, f->gpd_indirect },   { 5, 1, f->gpd_counts },
        { 6, 0, f->gpd_verts }
    };
    n = 0;
    for (k = 0; k < 20; k++) {
        bi[n].buffer = table[k].buf;
        bi[n].offset = 0;
        bi[n].range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = sets[table[k].set];
        w[n].dstBinding = table[k].binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    vkUpdateDescriptorSets(s->device, (uint32_t)n, w, 0, NULL);
    return true;
}

/*Load SPIR-V shaders and create 3D scenes + HUD pipelines.
 * Requires s->pipeline_layout, s->render_pass and s->swapchain_extent already valid.*/
static bool vk_create_pipelines(VkState *s) {
    VkShaderModule vert_mod;
    VkShaderModule frag_mod;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription bind_desc;
    VkVertexInputAttributeDescription attr_descs[3];
    VkPipelineVertexInputStateCreateInfo vi_ci;
    VkPipelineInputAssemblyStateCreateInfo ia_ci;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo vp_ci;
    VkPipelineRasterizationStateCreateInfo rs_ci;
    VkPipelineMultisampleStateCreateInfo ms_ci;
    VkPipelineDepthStencilStateCreateInfo ds_ci;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb_ci;
    VkGraphicsPipelineCreateInfo gp_ci;

    vert_mod = vk_load_shader(s, "shaders/shader.vert.spv");
    frag_mod = vk_load_shader(s, "shaders/shader.frag.spv");
    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) {
        printf("SPIR-V shader loading error\n");
        return false;
    }
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    memset(&bind_desc, 0, sizeof(bind_desc));
    bind_desc.binding = 0;
    bind_desc.stride = sizeof(VkVertex);
    bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(attr_descs, 0, sizeof(attr_descs));
    attr_descs[0].binding = 0;
    attr_descs[0].location = 0;
    attr_descs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[0].offset = 0;
    attr_descs[1].binding = 0;
    attr_descs[1].location = 1;
    attr_descs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attr_descs[1].offset = sizeof(float) * 3;
    attr_descs[2].binding = 0;
    attr_descs[2].location = 2;
    attr_descs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[2].offset = offsetof(VkVertex, normal);

    memset(&vi_ci, 0, sizeof(vi_ci));
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi_ci.vertexBindingDescriptionCount = 1;
    vi_ci.pVertexBindingDescriptions = &bind_desc;
    vi_ci.vertexAttributeDescriptionCount = 3;
    vi_ci.pVertexAttributeDescriptions = attr_descs;
    memset(&ia_ci, 0, sizeof(ia_ci));
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&viewport, 0, sizeof(viewport));
    viewport.width = (float)s->swapchain_extent.width;
    viewport.height = (float)s->swapchain_extent.height;
    viewport.maxDepth = 1.0f;
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent = s->swapchain_extent;
    memset(&vp_ci, 0, sizeof(vp_ci));
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.pViewports = &viewport;
    vp_ci.scissorCount = 1;
    vp_ci.pScissors = &scissor;
    memset(&rs_ci, 0, sizeof(rs_ci));
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rs_ci.lineWidth = 1.0f;
    rs_ci.cullMode = VK_CULL_MODE_NONE;
    rs_ci.frontFace = VK_FRONT_FACE_CLOCKWISE;
    memset(&ms_ci, 0, sizeof(ms_ci));
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds_ci, 0, sizeof(ds_ci));
    ds_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds_ci.depthTestEnable = VK_TRUE;
    ds_ci.depthWriteEnable = VK_TRUE;
    ds_ci.depthCompareOp = VK_COMPARE_OP_LESS;
    ds_ci.depthBoundsTestEnable = VK_FALSE;
    ds_ci.stencilTestEnable = VK_FALSE;

    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb_ci, 0, sizeof(cb_ci));
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &cba;

    /* --- 3D scene pipeline --- */
    memset(&gp_ci, 0, sizeof(gp_ci));
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDepthStencilState = &ds_ci;
    gp_ci.layout = s->pipeline_layout;
    VkPipelineRenderingCreateInfo pr_ci;
    memset(&pr_ci, 0, sizeof(pr_ci));
    pr_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr_ci.colorAttachmentCount = 1;
    pr_ci.pColorAttachmentFormats = &s->swapchain_format;
    pr_ci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    gp_ci.pNext = &pr_ci;
    gp_ci.renderPass = VK_NULL_HANDLE;
    gp_ci.subpass = 0;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp_ci, NULL, &s->pipeline) != VK_SUCCESS) {
        printf("Scene pipeline creation error\n");
        return false;
    }

    /* --- Pipeline HUD: depth test OFF, alpha blending ON --- */
    ds_ci.depthTestEnable  = VK_FALSE;
    ds_ci.depthWriteEnable = VK_FALSE;
    cba.blendEnable            = VK_TRUE;
    cba.srcColorBlendFactor    = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp           = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor    = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor    = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp           = VK_BLEND_OP_ADD;
    cb_ci.pAttachments         = &cba; /*reassign after modification*/
    memset(&gp_ci, 0, sizeof(gp_ci));
    gp_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount          = 2;
    gp_ci.pStages             = stages;
    gp_ci.pVertexInputState   = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState      = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState   = &ms_ci;
    gp_ci.pColorBlendState    = &cb_ci;
    gp_ci.pDepthStencilState  = &ds_ci;
    gp_ci.layout              = s->pipeline_layout;
    gp_ci.pNext               = &pr_ci;
    gp_ci.renderPass          = VK_NULL_HANDLE;
    gp_ci.subpass             = 0;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp_ci, NULL, &s->pipeline_hud) != VK_SUCCESS) {
        printf("HUD pipeline creation error\n");
        return false;
    }

    /*--- GPD scene pipeline: follows the render pass/swapchain lifecycle.
     * A failure downgrades to the legacy CPU-vertex path (the two
     * pipelines above are already in place) instead of failing init. ---*/
    if (s->gpu_driven && !vk_create_gpd_scene_pipeline(s)) {
        printf("[VK] GPD scene pipeline creation failed: "
               "falling back to the CPU-vertex path\n");
        s->gpu_driven = false;
    }

    vkDestroyShaderModule(s->device, vert_mod, NULL);
    vkDestroyShaderModule(s->device, frag_mod, NULL);
    return true;
}

/*Create swapchain, image views, depth targets, render passes, pipelines and
 * framebuffers for the given sizes.*/
static bool vk_create_swapchain(VkState *s, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *formats;
    uint32_t fmt_count;
    VkSwapchainCreateInfoKHR sc_ci;
    VkImageViewCreateInfo iv_ci;
    VkImageCreateInfo depth_image_ci;
    VkMemoryRequirements depth_mem_req;
    VkMemoryAllocateInfo depth_alloc_info;
    VkImageViewCreateInfo depth_view_ci;
    uint32_t i;

    s->swapchain_extent.width = width;
    s->swapchain_extent.height = height;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical_device, s->surface, &caps);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface, &fmt_count, NULL);
    if (fmt_count == 0) {
        printf("No surface format available.\n");
        return false;
    }
    formats = malloc(sizeof(VkSurfaceFormatKHR) * fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface, &fmt_count, formats);
    s->swapchain_format = formats[0].format;
    s->image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && s->image_count > caps.maxImageCount) {
        s->image_count = caps.maxImageCount;
    }

    memset(&sc_ci, 0, sizeof(sc_ci));
    sc_ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_ci.surface = s->surface;
    sc_ci.minImageCount = s->image_count;
    sc_ci.imageFormat = formats[0].format;
    sc_ci.imageColorSpace = formats[0].colorSpace;
    sc_ci.imageExtent = s->swapchain_extent;
    sc_ci.imageArrayLayers = 1;
    sc_ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_ci.preTransform = caps.currentTransform;
    sc_ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc_ci.clipped = VK_TRUE;
    free(formats);
    if (vkCreateSwapchainKHR(s->device, &sc_ci, NULL, &s->swapchain) != VK_SUCCESS) {
        printf("Swapchain creation error\n");
        return false;
    }
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->image_count, NULL);
    s->images = malloc(sizeof(VkImage) * s->image_count);
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->image_count, s->images);

    s->image_views = malloc(sizeof(VkImageView) * s->image_count);
    for (i = 0; i < s->image_count; i++) {
        memset(&iv_ci, 0, sizeof(iv_ci));
        iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_ci.image = s->images[i];
        iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_ci.format = s->swapchain_format;
        iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_ci.subresourceRange.levelCount = 1;
        iv_ci.subresourceRange.layerCount = 1;
        vkCreateImageView(s->device, &iv_ci, NULL, &s->image_views[i]);
    }

    memset(&depth_image_ci, 0, sizeof(depth_image_ci));
    depth_image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_image_ci.imageType = VK_IMAGE_TYPE_2D;
    depth_image_ci.extent.width = s->swapchain_extent.width;
    depth_image_ci.extent.height = s->swapchain_extent.height;
    depth_image_ci.extent.depth = 1;
    depth_image_ci.mipLevels = 1;
    depth_image_ci.arrayLayers = 1;
    depth_image_ci.format = VK_FORMAT_D32_SFLOAT;
    depth_image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_image_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(s->device, &depth_image_ci, NULL, &s->depth_image);

    vkGetImageMemoryRequirements(s->device, s->depth_image, &depth_mem_req);
    memset(&depth_alloc_info, 0, sizeof(depth_alloc_info));
    depth_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depth_alloc_info.allocationSize = depth_mem_req.size;
    depth_alloc_info.memoryTypeIndex = vk_find_memory_type(s, depth_mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(s->device, &depth_alloc_info, NULL, &s->depth_memory);
    vkBindImageMemory(s->device, s->depth_image, s->depth_memory, 0);

    memset(&depth_view_ci, 0, sizeof(depth_view_ci));
    depth_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_ci.image = s->depth_image;
    depth_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_ci.format = VK_FORMAT_D32_SFLOAT;
    depth_view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_ci.subresourceRange.baseMipLevel = 0;
    depth_view_ci.subresourceRange.levelCount = 1;
    depth_view_ci.subresourceRange.baseArrayLayer = 0;
    depth_view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(s->device, &depth_view_ci, NULL, &s->depth_image_view);

    if (!vk_create_pipelines(s)) {
        return false;
    }

    s->current_image = 0;
    return true;
}

/*Destroys resources dependent on window size:
 * framebuffers, pipeline, render pass, image views, swapchain and depth.
 * Device, instance, layout, command pool and vertex buffer remain intact.*/
static void vk_destroy_swapchain(VkState *s) {
    uint32_t i;
    if (s->pipeline) {
        vkDestroyPipeline(s->device, s->pipeline, NULL);
        s->pipeline = VK_NULL_HANDLE;
    }
    if (s->pipeline_hud) {
        vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
        s->pipeline_hud = VK_NULL_HANDLE;
    }
    if (s->gpd_scene_pipeline) {
        vkDestroyPipeline(s->device, s->gpd_scene_pipeline, NULL);
        s->gpd_scene_pipeline = VK_NULL_HANDLE;
    }
    if (s->image_views) {
        for (i = 0; i < s->image_count; i++) {
            vkDestroyImageView(s->device, s->image_views[i], NULL);
        }
        free(s->image_views);
        s->image_views = NULL;
    }
    if (s->images) {
        free(s->images);
        s->images = NULL;
    }
    if (s->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        s->swapchain = VK_NULL_HANDLE;
    }
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

/*Recreate the swapchain and all size-dependent assets.
 * Call from main thread after implicit vkDeviceWaitIdle.*/
bool vk_recreate_swapchain(VkState *s, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    vkDeviceWaitIdle(s->device);
    vk_destroy_swapchain(s);
    if (!vk_create_swapchain(s, width, height)) {
        printf("[VK] Swapchain recreation error %ux%u\n", width, height);
        return false;
    }
    printf("[VK] Swapchain recreated: %ux%u\n", width, height);
    return true;
}

bool vk_init(VkState *s) {
    VkApplicationInfo app_info;
    VkInstanceCreateInfo inst_ci;
    const char *exts[64];
    uint32_t ext_count;
    const char **glfw_exts;
    uint32_t dev_count;
    VkPhysicalDevice *devs;
    uint32_t qf_count;
    VkQueueFamilyProperties *qf_props;
    uint32_t i;
    float priority;
    VkDeviceQueueCreateInfo queue_ci;
    VkDeviceCreateInfo dev_ci;
    const char *dev_exts[1];
    VkSurfaceCapabilitiesKHR caps;
    VkPushConstantRange push_const;
    VkPipelineLayoutCreateInfo pl_ci;
    VkCommandPoolCreateInfo cp_ci;
    VkCommandBufferAllocateInfo cb_ai;
    VkSemaphoreCreateInfo sem_ci;
    VkFenceCreateInfo fen_ci;
    VkBufferCreateInfo buf_ci;
    VkMemoryRequirements mem_req;
    VkMemoryAllocateInfo mem_ai;

    memset(&app_info, 0, sizeof(app_info));
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "dragongl";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "dragongl";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_4;

    glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (i = 0; i < ext_count; i++) exts[i] = glfw_exts[i];

    /*--- Optional validation layers (L11) ------------------------------
     * The layers named in the standard VK_INSTANCE_LAYERS environment
     * variable (colon-separated, e.g. "VK_LAYER_KHRONOS_validation")
     * are enabled IF the driver provides them. This lets dev/CI builds
     * catch synchronization, memory and pipeline errors at init time
     * instead of as mysterious runtime corruption. With the variable
     * unset nothing is requested, so release users see no difference.
     * Unknown names are skipped with a warning: a missing layer must
     * never prevent the game from starting.*/
    const char *vk_wanted_layers = getenv("VK_INSTANCE_LAYERS");
    const char *vk_layers[16];
    uint32_t vk_layer_count = 0;
    VkLayerProperties *vk_avail = NULL;
    if (vk_wanted_layers && vk_wanted_layers[0]) {
        uint32_t avail_count = 0;
        vkEnumerateInstanceLayerProperties(&avail_count, NULL);
        if (avail_count > 0) {
            vk_avail = malloc(sizeof(VkLayerProperties) * avail_count);
            uint32_t got = avail_count;
            if (vk_avail &&
                vkEnumerateInstanceLayerProperties(&got, vk_avail) == VK_SUCCESS) {
                char *tok_copy = strdup(vk_wanted_layers);
                if (tok_copy) {
                    char *sp = NULL;
                    for (char *tok = strtok_r(tok_copy, ":", &sp);
                         tok; tok = strtok_r(NULL, ":", &sp)) {
                        bool have = false;
                        for (uint32_t li = 0; li < got; li++) {
                            if (strcmp(tok, vk_avail[li].layerName) == 0) {
                                if (vk_layer_count < 16)
                                    vk_layers[vk_layer_count++] =
                                        vk_avail[li].layerName;
                                have = true;
                                break;
                            }
                        }
                        if (!have)
                            printf("[VK] Warning: layer '%s' (VK_INSTANCE_LAYERS) "
                                   "not available - skipping.\n", tok);
                    }
                    free(tok_copy);
                }
            }
        }
    }
    if (vk_layer_count > 0)
        printf("[VK] Enabling %u validation layer(s)\n", vk_layer_count);

    memset(&inst_ci, 0, sizeof(inst_ci));
    inst_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_ci.pApplicationInfo = &app_info;
    inst_ci.enabledExtensionCount = ext_count;
    inst_ci.ppEnabledExtensionNames = exts;
    inst_ci.enabledLayerCount = vk_layer_count;
    inst_ci.ppEnabledLayerNames = vk_layers;
    if (vkCreateInstance(&inst_ci, NULL, &s->instance) != VK_SUCCESS) {
        printf("Error creating VkInstance\n");
        free(vk_avail);
        return false;
    }
    free(vk_avail);
    vk_avail = NULL;

    if (glfwCreateWindowSurface(s->instance, s->window, NULL, &s->surface) != VK_SUCCESS) {
        printf("Error creating VkSurface\n");
        return false;
    }

    vkEnumeratePhysicalDevices(s->instance, &dev_count, NULL);
    if (dev_count == 0) {
        printf("No Vulkan devices available.\n");
        return false;
    }
    devs = malloc(sizeof(VkPhysicalDevice) * dev_count);
    vkEnumeratePhysicalDevices(s->instance, &dev_count, devs);
    s->physical_device = devs[0];
    free(devs);

    vkGetPhysicalDeviceQueueFamilyProperties(s->physical_device, &qf_count, NULL);
    qf_props = malloc(sizeof(VkQueueFamilyProperties) * qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical_device, &qf_count, qf_props);
    /*
     * GPD: prefer a queue family supporting BOTH graphics and compute —
     * the GPU-driven path dispatches its compute work on the same queue
     * as the scene, so no cross-queue synchronization is ever needed.
     * If only graphics-only families exist, fall back to the legacy
     * CPU-vertex path. DRAGONGL_GPD=0 forces the legacy path anyway.
     */
    s->graphics_family = 0;
    s->gpu_driven = true;
    const char *env_gpd = getenv("DRAGONGL_GPD");
    if (env_gpd && env_gpd[0] == '0') {
        s->gpu_driven = false;
        printf("[VK] DRAGONGL_GPD=0: using the legacy CPU-vertex path\n");
    }
    {
        bool have_gc = false;
        for (i = 0; i < qf_count; i++) {
            if ((qf_props[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                s->graphics_family = i;
                have_gc = true;
                break;
            }
        }
        if (!have_gc) {
            for (i = 0; i < qf_count; i++) {
                if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    s->graphics_family = i;
                    break;
                }
            }
            if (s->gpu_driven) {
                printf("[VK] No graphics+compute queue family: "
                       "falling back to the CPU-vertex path\n");
                s->gpu_driven = false;
            }
        }
    }
    free(qf_props);

    priority = 1.0f;
    memset(&queue_ci, 0, sizeof(queue_ci));
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = s->graphics_family;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;

    dev_exts[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    VkPhysicalDeviceFeatures dev_features;
    memset(&dev_features, 0, sizeof(dev_features));
    dev_features.fillModeNonSolid = VK_TRUE;
    dev_features.multiDrawIndirect = VK_TRUE;

    VkPhysicalDeviceVulkan11Features features11;
    memset(&features11, 0, sizeof(features11));
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

    VkPhysicalDeviceVulkan12Features features12;
    memset(&features12, 0, sizeof(features12));
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.drawIndirectCount = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.pNext = &features11;

    VkPhysicalDeviceVulkan13Features features13;
    memset(&features13, 0, sizeof(features13));
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = VK_TRUE;
    features13.pNext = &features12;

    VkPhysicalDeviceVulkan14Features features14;
    memset(&features14, 0, sizeof(features14));
    features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features14.pushDescriptor = VK_TRUE;
    features14.pNext = &features13;

    VkPhysicalDeviceFeatures2 features2;
    memset(&features2, 0, sizeof(features2));
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features = dev_features;
    features2.pNext = &features14;

    memset(&dev_ci, 0, sizeof(dev_ci));
    dev_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_ci.queueCreateInfoCount = 1;
    dev_ci.pQueueCreateInfos = &queue_ci;
    dev_ci.enabledExtensionCount = 1;
    dev_ci.ppEnabledExtensionNames = dev_exts;
    dev_ci.pEnabledFeatures = NULL;
    dev_ci.pNext = &features2;
    if (vkCreateDevice(s->physical_device, &dev_ci, NULL, &s->device) != VK_SUCCESS) {
        printf("VkDevice creation error\n");
        return false;
    }
    vkGetDeviceQueue(s->device, s->graphics_family, 0, &s->graphics_queue);
    s->present_queue = s->graphics_queue;

    /* Pipeline layout (independent of size) */
    memset(&push_const, 0, sizeof(push_const));
    push_const.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_const.offset = 0;
    push_const.size = sizeof(float) * 20;

    memset(&pl_ci, 0, sizeof(pl_ci));
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_const;
    vkCreatePipelineLayout(s->device, &pl_ci, NULL, &s->pipeline_layout);

    /*GPD: descriptor layouts, compute pipelines and the descriptor
     * pool (size-independent, live for the whole device). The GPD scene
     * pipeline is created inside vk_create_swapchain (it is
     * size-dependent and follows that lifecycle). A failure downgrades
     * to the legacy CPU-vertex path instead of killing the client.*/
    if (s->gpu_driven) {
        if (!vk_create_gpd_init(s)) {
            printf("[VK] GPD initialization failed: "
                   "falling back to the CPU-vertex path\n");
            s->gpu_driven = false;
        }
    }

    /*Initial Swapchain: Use the current extent of the surface*/
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical_device, s->surface, &caps);
    {
        uint32_t w = caps.currentExtent.width;
        uint32_t h = caps.currentExtent.height;
        if (w == 0xFFFFFFFF) {
            w = 800;
            h = 800;
        }
        if (!vk_create_swapchain(s, w, h)) {
            return false;
        }
    }

    memset(&cp_ci, 0, sizeof(cp_ci));
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = s->graphics_family;
    vkCreateCommandPool(s->device, &cp_ci, NULL, &s->command_pool);
    memset(&cb_ai, 0, sizeof(cb_ai));
    cb_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_ai.commandPool = s->command_pool;
    cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
    if (vkAllocateCommandBuffers(s->device, &cb_ai, command_buffers) != VK_SUCCESS) {
        printf("[VK] Command buffer allocation error\n");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        s->frames[i].command_buffer = command_buffers[i];
    }

    /*One semaphore pair + fence per in-flight frame (triple buffering);
     * fences start SIGNALED so the first wait per slot returns at once.*/
    memset(&sem_ci, 0, sizeof(sem_ci));
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    memset(&fen_ci, 0, sizeof(fen_ci));
    fen_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fen_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    s->current_frame = 0;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_image[i]);
        vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_render[i]);
        vkCreateFence(s->device, &fen_ci, NULL, &s->fences[i]);
    }

    /*2D HUD Vertices:
     * - Minimap: MINIMAP_BUF_SIZE^2 * 6 ≈ 81*81*6 = 39366
     * - Text (5x7 font): ~200 chars * 35 pixels/char * 6 vertices = 42000
     * - Quad boss trophies/equip: ~100 * 6 = 600
     * - Total rounded with margin*/
    uint32_t hud_verts = 39366 + 42000 + 2000;
    if (s->gpu_driven) {
        /*GPD: the per-frame HOST buffer holds the 2D HUD only — the 3D
         * scene is generated by the compute pass into the device-local
         * gpd_verts SSBO (VKD_VERTEX_CAPACITY vertices, see
         * render_vk.h). Host memory drops from ~400 MB (3 x ~136 MB)
         * to ~12 MB.*/
        s->max_vertices = VKD_HUD_VERTS;
    } else {
        /*3D scene vertices: 201x201 tiles * 36 vertices/cube + 36 for the player + NPC*/
        uint32_t scene_verts = 300 * 300 * 36 + 36 + CLIENT_MAX_ENTITIES * 36 + MAX_PARTICLES * 36;
        s->max_vertices = scene_verts + hud_verts;
    }
    memset(&buf_ci, 0, sizeof(buf_ci));
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = sizeof(VkVertex) * s->max_vertices;
    buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkFrameResources *frame = &s->frames[i];

        if (vkCreateBuffer(s->device, &buf_ci, NULL, &frame->vertex_buffer) != VK_SUCCESS) {
            printf("[VK] Vertex buffer creation error (frame %u)\n", i);
            return false;
        }

        vkGetBufferMemoryRequirements(s->device, frame->vertex_buffer, &mem_req);
        memset(&mem_ai, 0, sizeof(mem_ai));
        mem_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mem_ai.allocationSize = mem_req.size;
        mem_ai.memoryTypeIndex = vk_find_memory_type(s, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(s->device, &mem_ai, NULL, &frame->vertex_memory) != VK_SUCCESS) {
            printf("[VK] Vertex memory allocation error (frame %u)\n", i);
            return false;
        }
        if (vkBindBufferMemory(s->device, frame->vertex_buffer, frame->vertex_memory, 0) != VK_SUCCESS) {
            printf("[VK] Vertex buffer bind error (frame %u)\n", i);
            return false;
        }
        if (vkMapMemory(s->device, frame->vertex_memory, 0, mem_req.size, 0,
                        &frame->mapped_vertex_data) != VK_SUCCESS) {
            printf("[VK] Vertex buffer mapping error (frame %u)\n", i);
            return false;
        }

        /*GPD: the GPU-driven ring for this slot — instance SSBOs (host
         * visible, persistent mapping), the device-local vertex SSBO,
         * the indirect command buffer and the atomic counter — plus the
         * five per-slot descriptor sets (written once: the buffers are
         * stable for the lifetime of the device).*/
        if (s->gpu_driven) {
            if (!vk_create_gpd_frame_buffers(s, i)) {
                printf("[VK] GPD frame buffer creation error (frame %u)\n", i);
                return false;
            }
        }
    }

    printf("[VK] Vulkan initialization complete.\n");
    return true;
}

void vk_cleanup(VkState *s) {
    uint32_t i;
    vkDeviceWaitIdle(s->device);
    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (s->frames[i].mapped_vertex_data) {
            vkUnmapMemory(s->device, s->frames[i].vertex_memory);
            s->frames[i].mapped_vertex_data = NULL;
        }
        if (s->frames[i].vertex_buffer) {
            vkDestroyBuffer(s->device, s->frames[i].vertex_buffer, NULL);
            s->frames[i].vertex_buffer = VK_NULL_HANDLE;
        }
        if (s->frames[i].vertex_memory) {
            vkFreeMemory(s->device, s->frames[i].vertex_memory, NULL);
            s->frames[i].vertex_memory = VK_NULL_HANDLE;
        }

        /*GPD ring: unmap, then destroy the 8 per-slot buffers.*/
        if (s->frames[i].gpd_map_ptr)  vkUnmapMemory(s->device, s->frames[i].gpd_map_mem);
        if (s->frames[i].gpd_dyn_ptr)  vkUnmapMemory(s->device, s->frames[i].gpd_dyn_mem);
        if (s->frames[i].gpd_ori_ptr)  vkUnmapMemory(s->device, s->frames[i].gpd_ori_mem);
        if (s->frames[i].gpd_ind_ptr)  vkUnmapMemory(s->device, s->frames[i].gpd_ind_mem);
        if (s->frames[i].gpd_cnt_ptr)  vkUnmapMemory(s->device, s->frames[i].gpd_cnt_mem);
        if (s->frames[i].gpd_map_inst)    { vkDestroyBuffer(s->device, s->frames[i].gpd_map_inst, NULL);    s->frames[i].gpd_map_inst = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_dyn_inst)    { vkDestroyBuffer(s->device, s->frames[i].gpd_dyn_inst, NULL);    s->frames[i].gpd_dyn_inst = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_oriented)    { vkDestroyBuffer(s->device, s->frames[i].gpd_oriented, NULL);    s->frames[i].gpd_oriented = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_map_visible) { vkDestroyBuffer(s->device, s->frames[i].gpd_map_visible, NULL); s->frames[i].gpd_map_visible = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_dyn_visible) { vkDestroyBuffer(s->device, s->frames[i].gpd_dyn_visible, NULL); s->frames[i].gpd_dyn_visible = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_verts)       { vkDestroyBuffer(s->device, s->frames[i].gpd_verts, NULL);       s->frames[i].gpd_verts = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_indirect)    { vkDestroyBuffer(s->device, s->frames[i].gpd_indirect, NULL);    s->frames[i].gpd_indirect = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_counts)      { vkDestroyBuffer(s->device, s->frames[i].gpd_counts, NULL);      s->frames[i].gpd_counts = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_map_mem)     { vkFreeMemory(s->device, s->frames[i].gpd_map_mem, NULL);     s->frames[i].gpd_map_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_dyn_mem)     { vkFreeMemory(s->device, s->frames[i].gpd_dyn_mem, NULL);     s->frames[i].gpd_dyn_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_ori_mem)     { vkFreeMemory(s->device, s->frames[i].gpd_ori_mem, NULL);     s->frames[i].gpd_ori_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_mvis_mem)    { vkFreeMemory(s->device, s->frames[i].gpd_mvis_mem, NULL);    s->frames[i].gpd_mvis_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_dvis_mem)    { vkFreeMemory(s->device, s->frames[i].gpd_dvis_mem, NULL);    s->frames[i].gpd_dvis_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_vert_mem)    { vkFreeMemory(s->device, s->frames[i].gpd_vert_mem, NULL);    s->frames[i].gpd_vert_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_ind_mem)     { vkFreeMemory(s->device, s->frames[i].gpd_ind_mem, NULL);     s->frames[i].gpd_ind_mem = VK_NULL_HANDLE; }
        if (s->frames[i].gpd_cnt_mem)     { vkFreeMemory(s->device, s->frames[i].gpd_cnt_mem, NULL);     s->frames[i].gpd_cnt_mem = VK_NULL_HANDLE; }
    }
    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(s->device, s->fences[i], NULL);
        vkDestroySemaphore(s->device, s->sem_render[i], NULL);
        vkDestroySemaphore(s->device, s->sem_image[i], NULL);
    }
    vkDestroyCommandPool(s->device, s->command_pool, NULL);

    /*GPD persistent resources (the per-slot descriptor sets die with
     * the pool).*/
    if (s->gpd_desc_pool) vkDestroyDescriptorPool(s->device, s->gpd_desc_pool, NULL);
    if (s->gpd_ds_layout_cull) vkDestroyDescriptorSetLayout(s->device, s->gpd_ds_layout_cull, NULL);
    if (s->gpd_ds_layout_exp) vkDestroyDescriptorSetLayout(s->device, s->gpd_ds_layout_exp, NULL);
    if (s->gpd_ds_layout_exp_ori) vkDestroyDescriptorSetLayout(s->device, s->gpd_ds_layout_exp_ori, NULL);
    if (s->gpd_ds_layout_final) vkDestroyDescriptorSetLayout(s->device, s->gpd_ds_layout_final, NULL);
    if (s->gpd_scene_ds_layout) vkDestroyDescriptorSetLayout(s->device, s->gpd_scene_ds_layout, NULL);
    if (s->gpd_pipe_cull_map) vkDestroyPipeline(s->device, s->gpd_pipe_cull_map, NULL);
    if (s->gpd_pipe_cull_dyn) vkDestroyPipeline(s->device, s->gpd_pipe_cull_dyn, NULL);
    if (s->gpd_pipe_exp_map) vkDestroyPipeline(s->device, s->gpd_pipe_exp_map, NULL);
    if (s->gpd_pipe_exp_dyn) vkDestroyPipeline(s->device, s->gpd_pipe_exp_dyn, NULL);
    if (s->gpd_pipe_exp_ori) vkDestroyPipeline(s->device, s->gpd_pipe_exp_ori, NULL);
    if (s->gpd_pipe_final) vkDestroyPipeline(s->device, s->gpd_pipe_final, NULL);
    if (s->gpd_pl_cull) vkDestroyPipelineLayout(s->device, s->gpd_pl_cull, NULL);
    if (s->gpd_pl_exp) vkDestroyPipelineLayout(s->device, s->gpd_pl_exp, NULL);
    if (s->gpd_pl_exp_ori) vkDestroyPipelineLayout(s->device, s->gpd_pl_exp_ori, NULL);
    if (s->gpd_pl_final) vkDestroyPipelineLayout(s->device, s->gpd_pl_final, NULL);
    if (s->gpd_scene_layout) vkDestroyPipelineLayout(s->device, s->gpd_scene_layout, NULL);
    if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
    if (s->pipeline_hud) vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
    if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
    if (s->image_views) {
        for (i = 0; i < s->image_count; i++) {
            vkDestroyImageView(s->device, s->image_views[i], NULL);
        }
        free(s->image_views);
        s->image_views = NULL;
    }
    if (s->images) {
        free(s->images);
        s->images = NULL;
    }
    vkDestroyImageView(s->device, s->depth_image_view, NULL);
    vkDestroyImage(s->device, s->depth_image, NULL);
    vkFreeMemory(s->device, s->depth_memory, NULL);
    vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
    vkDestroyDevice(s->device, NULL);
    vkDestroySurfaceKHR(s->instance, s->surface, NULL);
    vkDestroyInstance(s->instance, NULL);
}
