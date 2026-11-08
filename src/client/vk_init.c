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
#include "client_state.h"
#include "client_particles.h"
#include "client_minimap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*Index of a memory type allowed by `filter` with all of `props`, or
 *UINT32_MAX if the device has none.*/
static uint32_t vk_find_memory_type(VkState *s, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(s->physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/*Create a buffer with its own memory: host visible and coherent (and
 *persistently mapped into *mapped) when `host`, device local otherwise.*/
static bool vk_create_buffer(VkState *s, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                             VkBuffer *buf, VkDeviceMemory *mem, void **mapped) {
    VkBufferCreateInfo bc = {0};
    bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bc.size = size;
    bc.usage = usage;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s->device, &bc, NULL, buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s->device, *buf, &req);
    uint32_t type = vk_find_memory_type(
        s, req.memoryTypeBits,
        host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(s->device, &ai, NULL, mem) != VK_SUCCESS)
        return false;
    if (vkBindBufferMemory(s->device, *buf, *mem, 0) != VK_SUCCESS)
        return false;
    if (host && mapped && vkMapMemory(s->device, *mem, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS)
        return false;
    return true;
}

/*Read a SPIR-V file of shaders/ (name like "shader.frag.spv"). Searched:
 *  1. $DRAGONGL_SHADER_DIR, if set;
 *  2. <directory of the executable>/shaders (build tree: build/shaders);
 *  3. /usr/share/dragongl/shaders (RPM install).
 *The current directory is not searched: what the client runs must not
 *depend on where it was started from.*/
static VkShaderModule vk_load_shader(VkState *s, const char *name) {
    char dirs[3][4096];
    int n_dirs = 0;
    const char *env = getenv("DRAGONGL_SHADER_DIR");
    if (env && env[0])
        snprintf(dirs[n_dirs++], sizeof(dirs[0]), "%s", env);
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
    snprintf(dirs[n_dirs++], sizeof(dirs[0]), "/usr/share/dragongl/shaders");

    FILE *f = NULL;
    char path[sizeof(dirs[0]) + 64];
    for (int d = 0; d < n_dirs && !f; d++) {
        if (snprintf(path, sizeof(path), "%.4095s/%.60s", dirs[d], name) < (int)sizeof(path))
            f = fopen(path, "rb");
    }
    if (!f) {
        printf("[VK] Shader %s not found (DRAGONGL_SHADER_DIR, next to the executable, "
               "/usr/share/dragongl/shaders).\n", name);
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = VK_NULL_HANDLE;
    long size = -1;
    uint32_t *code = NULL;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size > 0 && size % 4 == 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (code = malloc((size_t)size)) != NULL && fread(code, 1, (size_t)size, f) == (size_t)size) {
        VkShaderModuleCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = (size_t)size;
        ci.pCode = code;
        if (vkCreateShaderModule(s->device, &ci, NULL, &mod) != VK_SUCCESS)
            mod = VK_NULL_HANDLE;
    }
    if (mod == VK_NULL_HANDLE)
        printf("[VK] Invalid shader %s\n", path);
    free(code);
    fclose(f);
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
static bool gpd_create_compute_pipeline(VkState *s, const char *spv_name,
                                        VkPipelineLayout layout, VkPipeline *out) {
    VkShaderModule mod = vk_load_shader(s, spv_name);
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
        { "gpd_cull_map.comp.spv",         s->gpd_pl_cull,    &s->gpd_pipe_cull_map },
        { "gpd_cull_dyn.comp.spv",         s->gpd_pl_cull,    &s->gpd_pipe_cull_dyn },
        { "gpd_expand_map.comp.spv",       s->gpd_pl_exp,     &s->gpd_pipe_exp_map },
        { "gpd_expand_dyn.comp.spv",       s->gpd_pl_exp,     &s->gpd_pipe_exp_dyn },
        { "gpd_expand_oriented.comp.spv",  s->gpd_pl_exp_ori, &s->gpd_pipe_exp_ori },
        { "gpd_final.comp.spv",            s->gpd_pl_final,   &s->gpd_pipe_final }
    };
    for (p = 0; p < 6; p++) {
        if (!gpd_create_compute_pipeline(s, pipes[p].spv, pipes[p].pl, pipes[p].out)) return false;
    }
    return true;
}

/*The scene pipeline: no vertex input — the vertex shader reads the
 * compute-generated vertices from the storage buffer (binding 0).*/
static bool vk_create_gpd_scene_pipeline(VkState *s) {
    VkShaderModule vert_mod;
    VkShaderModule frag_mod;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi_ci;
    VkPipelineInputAssemblyStateCreateInfo ia_ci;
    VkPipelineViewportStateCreateInfo vp_ci;
    VkPipelineRasterizationStateCreateInfo rs_ci;
    VkPipelineMultisampleStateCreateInfo ms_ci;
    VkPipelineDepthStencilStateCreateInfo ds_ci;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb_ci;
    VkGraphicsPipelineCreateInfo gp_ci;

    vert_mod = vk_load_shader(s, "gpd_scene.vert.spv");
    frag_mod = vk_load_shader(s, "shader.frag.spv");
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
    /*Viewport and scissor are dynamic (set per frame): the pipeline does
     *not depend on the window size and survives a resize.*/
    memset(&vp_ci, 0, sizeof(vp_ci));
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;
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
    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_ci = {0};
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dyn_states;
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
    gp_ci.pDynamicState = &dyn_ci;
    VkPipelineRenderingCreateInfo pr_ci;
    memset(&pr_ci, 0, sizeof(pr_ci));
    pr_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr_ci.colorAttachmentCount = 1;
    pr_ci.pColorAttachmentFormats = &s->swapchain_format;
    pr_ci.depthAttachmentFormat = s->depth_format;

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
 *   gpd_counts      GPU fill +   cull/expand/final (GPU accumulate)      */
static bool vk_create_gpd_frame_buffers(VkState *s, uint32_t i) {
    VkFrameResources *f = &s->frames[i];
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
    bufs[6] = &f->gpd_indirect;    mems[6] = &f->gpd_ind_mem;    ptrs[6] = NULL;
    bufs[7] = &f->gpd_counts;      mems[7] = &f->gpd_cnt_mem;    ptrs[7] = NULL;
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
    /*Reset with vkCmdFillBuffer at the start of every frame.*/
    usages[7] = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    host[0] = true;  host[1] = true;  host[2] = true;
    host[3] = false; /*device-local: GPU-only (cull output)*/
    host[4] = false;
    host[5] = false; /*device-local: GPU writes it, the CPU never does*/
    host[6] = false; /*indirect command and counters: written and read */
    host[7] = false; /*by the GPU only                                  */

    for (d = 0; d < 8; d++) {
        if (!vk_create_buffer(s, sizes[d], usages[d], host[d], bufs[d], mems[d], ptrs[d]))
            return false;
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

/*The 2D HUD pipeline (host vertex buffer, alpha blending, no depth test)
 *and the scene pipeline. They depend on the colour and depth formats
 *only — viewport and scissor are dynamic — so a resize keeps them.*/
static bool vk_create_pipelines(VkState *s) {
    VkShaderModule vert_mod = vk_load_shader(s, "shader.vert.spv");
    VkShaderModule frag_mod = vk_load_shader(s, "shader.frag.spv");
    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) {
        if (vert_mod) vkDestroyShaderModule(s->device, vert_mod, NULL);
        if (frag_mod) vkDestroyShaderModule(s->device, frag_mod, NULL);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{0}, {0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind_desc = {0};
    bind_desc.binding = 0;
    bind_desc.stride = sizeof(VkVertex);
    bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr_descs[3] = {{0}, {0}, {0}};
    attr_descs[0].location = 0;
    attr_descs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[0].offset = offsetof(VkVertex, pos);
    attr_descs[1].location = 1;
    attr_descs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attr_descs[1].offset = offsetof(VkVertex, color);
    attr_descs[2].location = 2;
    attr_descs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[2].offset = offsetof(VkVertex, normal);
    VkPipelineVertexInputStateCreateInfo vi_ci = {0};
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi_ci.vertexBindingDescriptionCount = 1;
    vi_ci.pVertexBindingDescriptions = &bind_desc;
    vi_ci.vertexAttributeDescriptionCount = 3;
    vi_ci.pVertexAttributeDescriptions = attr_descs;

    VkPipelineInputAssemblyStateCreateInfo ia_ci = {0};
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp_ci = {0};
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs_ci = {0};
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rs_ci.lineWidth = 1.0f;
    rs_ci.cullMode = VK_CULL_MODE_NONE;
    rs_ci.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms_ci = {0};
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds_ci = {0};
    ds_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb_ci = {0};
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &cba;
    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_ci = {0};
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo pr_ci = {0};
    pr_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr_ci.colorAttachmentCount = 1;
    pr_ci.pColorAttachmentFormats = &s->swapchain_format;
    pr_ci.depthAttachmentFormat = s->depth_format;

    VkGraphicsPipelineCreateInfo gp_ci = {0};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.pNext = &pr_ci;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pDepthStencilState = &ds_ci;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDynamicState = &dyn_ci;
    gp_ci.layout = s->pipeline_layout;
    bool ok = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp_ci, NULL,
                                        &s->pipeline_hud) == VK_SUCCESS;
    vkDestroyShaderModule(s->device, vert_mod, NULL);
    vkDestroyShaderModule(s->device, frag_mod, NULL);
    if (!ok) {
        printf("[VK] HUD pipeline creation error\n");
        return false;
    }
    if (!vk_create_gpd_scene_pipeline(s)) {
        printf("[VK] Scene pipeline creation error\n");
        return false;
    }
    s->pipelines_format = s->swapchain_format;
    return true;
}

static void vk_destroy_pipelines(VkState *s) {
    if (s->pipeline_hud) {
        vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
        s->pipeline_hud = VK_NULL_HANDLE;
    }
    if (s->gpd_scene_pipeline) {
        vkDestroyPipeline(s->device, s->gpd_scene_pipeline, NULL);
        s->gpd_scene_pipeline = VK_NULL_HANDLE;
    }
}

/*Surface format: 8-bit sRGB (the colours of the game are tuned for it),
 *else whatever the surface lists first.*/
static VkSurfaceFormatKHR choose_surface_format(VkState *s) {
    VkSurfaceFormatKHR chosen = {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface, &n, NULL);
    VkSurfaceFormatKHR *f = n ? malloc(sizeof(*f) * n) : NULL;
    if (!f)
        return chosen;
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
    if (!found && n > 0 && f[0].format != VK_FORMAT_UNDEFINED)
        chosen = f[0];
    free(f);
    return chosen;
}

/*FIFO (vsync) is always available. DRAGONGL_VSYNC=0 asks for MAILBOX,
 *then IMMEDIATE, if the surface offers them.*/
static VkPresentModeKHR choose_present_mode(VkState *s) {
    const char *vs = getenv("DRAGONGL_VSYNC");
    if (!vs || vs[0] != '0')
        return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface, &n, NULL);
    VkPresentModeKHR *m = n ? malloc(sizeof(*m) * n) : NULL;
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    if (m) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface, &n, m);
        for (uint32_t i = 0; i < n; i++)
            if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                chosen = m[i];
        for (uint32_t i = 0; i < n; i++)
            if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                chosen = m[i];
        free(m);
    }
    return chosen;
}

/*The first depth format the device can render to.*/
static bool choose_depth_format(VkState *s) {
    const VkFormat candidates[4] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (int i = 0; i < 4; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s->physical_device, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s->depth_format = candidates[i];
            s->depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (candidates[i] == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                candidates[i] == VK_FORMAT_D24_UNORM_S8_UINT)
                s->depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
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
    if (vkCreateImage(s->device, &ci, NULL, &s->depth_image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s->device, s->depth_image, &req);
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        vk_find_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s->device, &ai, NULL, &s->depth_memory) != VK_SUCCESS ||
        vkBindImageMemory(s->device, s->depth_image, s->depth_memory, 0) != VK_SUCCESS)
        return false;
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
static void vk_destroy_swapchain_resources(VkState *s) {
    if (s->image_views) {
        for (uint32_t i = 0; i < s->image_count; i++)
            if (s->image_views[i])
                vkDestroyImageView(s->device, s->image_views[i], NULL);
        free(s->image_views);
        s->image_views = NULL;
    }
    if (s->sem_render) {
        for (uint32_t i = 0; i < s->image_count; i++)
            if (s->sem_render[i])
                vkDestroySemaphore(s->device, s->sem_render[i], NULL);
        free(s->sem_render);
        s->sem_render = NULL;
    }
    free(s->images);
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
static bool vk_create_swapchain(VkState *s, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical_device, s->surface, &caps) !=
        VK_SUCCESS)
        return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width;
        extent.height = height;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (extent.width == 0 || extent.height == 0)
        return false; /* minimized: the caller waits */

    VkSurfaceFormatKHR fmt = choose_surface_format(s);
    uint32_t min_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && min_images > caps.maxImageCount)
        min_images = caps.maxImageCount;

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
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(s->device, old, NULL);
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
    s->images = calloc(count, sizeof(VkImage));
    s->image_views = calloc(count, sizeof(VkImageView));
    s->sem_render = calloc(count, sizeof(VkSemaphore));
    if (!s->images || !s->image_views || !s->sem_render)
        return false;
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
            vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_render[i]) != VK_SUCCESS)
            return false;
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
    if (width == 0 || height == 0)
        return true;
    vkDeviceWaitIdle(s->device);
    vk_destroy_swapchain_resources(s);
    if (!vk_create_swapchain(s, width, height)) {
        printf("[VK] Swapchain recreation error %ux%u\n", width, height);
        return false;
    }
    if (s->swapchain_format != s->pipelines_format) {
        vk_destroy_pipelines(s);
        if (!vk_create_pipelines(s))
            return false;
    }
    return true;
}

/* --- Instance and device -------------------------------------------------- */

static bool vk_create_instance(VkState *s) {
    uint32_t loader_version = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enum_version =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (enum_version)
        enum_version(&loader_version);
    if (loader_version < VK_API_VERSION_1_3) {
        printf("[VK] The Vulkan loader supports %u.%u: Dragon GL needs Vulkan 1.3.\n",
               VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version));
        return false;
    }

    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "dragongl";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.pEngineName = "dragongl";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    uint32_t ext_count = 0;
    const char **glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    if (!glfw_exts) {
        printf("[VK] GLFW found no Vulkan surface support for this window system.\n");
        return false;
    }

    /*Optional layers: the names in VK_INSTANCE_LAYERS (colon-separated,
     *e.g. VK_LAYER_KHRONOS_validation) that the system provides. Unknown
     *names are skipped with a warning.*/
    const char *wanted = getenv("VK_INSTANCE_LAYERS");
    const char *layers[16];
    uint32_t layer_count = 0;
    VkLayerProperties *avail = NULL;
    uint32_t avail_count = 0;
    if (wanted && wanted[0] && vkEnumerateInstanceLayerProperties(&avail_count, NULL) == VK_SUCCESS &&
        avail_count > 0 && (avail = malloc(sizeof(*avail) * avail_count)) != NULL &&
        vkEnumerateInstanceLayerProperties(&avail_count, avail) == VK_SUCCESS) {
        char *copy = strdup(wanted);
        char *sp = NULL;
        for (char *tok = copy ? strtok_r(copy, ":", &sp) : NULL; tok; tok = strtok_r(NULL, ":", &sp)) {
            bool have = false;
            for (uint32_t li = 0; li < avail_count && !have; li++) {
                if (strcmp(tok, avail[li].layerName) == 0) {
                    have = true;
                    if (layer_count < 16)
                        layers[layer_count++] = avail[li].layerName;
                }
            }
            if (!have)
                printf("[VK] Warning: layer '%s' (VK_INSTANCE_LAYERS) not available - skipping.\n", tok);
        }
        free(copy);
    }
    if (layer_count > 0)
        printf("[VK] Enabling %u layer(s)\n", layer_count);

    VkInstanceCreateInfo inst_ci = {0};
    inst_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_ci.pApplicationInfo = &app_info;
    inst_ci.enabledExtensionCount = ext_count;
    inst_ci.ppEnabledExtensionNames = glfw_exts;
    inst_ci.enabledLayerCount = layer_count;
    inst_ci.ppEnabledLayerNames = layers;
    VkResult r = vkCreateInstance(&inst_ci, NULL, &s->instance);
    free(avail);
    if (r != VK_SUCCESS) {
        printf("[VK] Error creating VkInstance (%d)\n", r);
        return false;
    }
    return true;
}

static const char *device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "software";
    default:                                     return "other";
    }
}

/*Why `dev` cannot run the game (NULL if it can); *family receives a
 *queue family with graphics, compute and presentation to the surface.*/
static const char *device_unsuitable(VkState *s, VkPhysicalDevice dev, uint32_t *family) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    if (props.apiVersion < VK_API_VERSION_1_3)
        return "Vulkan 1.3 not supported";

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, NULL, &n, NULL);
    VkExtensionProperties *exts = n ? malloc(sizeof(*exts) * n) : NULL;
    bool swapchain = false;
    if (exts) {
        vkEnumerateDeviceExtensionProperties(dev, NULL, &n, exts);
        for (uint32_t i = 0; i < n; i++)
            if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                swapchain = true;
        free(exts);
    }
    if (!swapchain)
        return "no VK_KHR_swapchain";

    VkPhysicalDeviceVulkan13Features f13 = {0};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {0};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(dev, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2)
        return "no dynamicRendering/synchronization2";

    /*One queue does everything: the compute passes, the scene, the
     *presentation — no cross-queue synchronization.*/
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, NULL);
    VkQueueFamilyProperties *qf = n ? malloc(sizeof(*qf) * n) : NULL;
    bool found = false;
    if (qf) {
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, qf);
        const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        for (uint32_t i = 0; i < n && !found; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, s->surface, &present);
            if ((qf[i].queueFlags & need) == need && present) {
                *family = i;
                found = true;
            }
        }
        free(qf);
    }
    if (!found)
        return "no graphics+compute queue that can present to the window";

    uint32_t formats = 0, modes = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, s->surface, &formats, NULL);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, s->surface, &modes, NULL);
    if (formats == 0 || modes == 0)
        return "cannot present to the window";
    return NULL;
}

/*Device-local memory, in MiB (tie-breaker between similar devices).*/
static uint64_t device_local_mib(VkPhysicalDevice dev) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    uint64_t best = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mp.memoryHeaps[i].size > best)
            best = mp.memoryHeaps[i].size;
    return best >> 20;
}

/*Pick the GPU: every suitable device is scored (discrete > integrated >
 *virtual > software, then device-local memory). DRAGONGL_GPU=<index>
 *or a part of the device name selects one explicitly.*/
static bool vk_pick_device(VkState *s) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(s->instance, &n, NULL) != VK_SUCCESS || n == 0) {
        printf("[VK] No Vulkan device found.\n");
        return false;
    }
    VkPhysicalDevice *devs = malloc(sizeof(*devs) * n);
    if (!devs)
        return false;
    vkEnumeratePhysicalDevices(s->instance, &n, devs);
    const char *want = getenv("DRAGONGL_GPU");
    char *end = NULL;
    long want_index = (want && want[0]) ? strtol(want, &end, 10) : -1;
    bool want_is_index = want && want[0] && end && *end == '\0';

    int64_t best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        uint32_t family = 0;
        const char *why = device_unsuitable(s, devs[i], &family);
        printf("[VK] GPU %u: %s (%s)%s%s\n", i, props.deviceName, device_type_name(props.deviceType),
               why ? " - unusable: " : "", why ? why : "");
        if (why)
            continue;
        int64_t score;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 4000000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3000000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 2000000; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            score = 1000000; break;
        default:                                     score = 0;       break;
        }
        uint64_t mib = device_local_mib(devs[i]);
        score += (int64_t)(mib > 999999 ? 999999 : mib);
        if (want && want[0]) {
            bool match = want_is_index ? (want_index == (long)i)
                                       : (strstr(props.deviceName, want) != NULL);
            if (match)
                score += 100000000;
        }
        if (score > best_score) {
            best_score = score;
            s->physical_device = devs[i];
            s->graphics_family = family;
        }
    }
    free(devs);
    if (best_score < 0) {
        printf("[VK] No usable Vulkan 1.3 device.\n");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s->physical_device, &props);
    printf("[VK] Using %s (Vulkan %u.%u, queue family %u)\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           s->graphics_family);
    if (want && want[0] && best_score < 100000000)
        printf("[VK] Warning: DRAGONGL_GPU=%s matches no usable device.\n", want);
    return true;
}

static bool vk_create_device(VkState *s) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {0};
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = s->graphics_family;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;

    /*Only what the renderer uses: dynamic rendering (no render passes)
     *and synchronization2 (vkCmdPipelineBarrier2).*/
    VkPhysicalDeviceVulkan13Features f13 = {0};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    const char *dev_exts[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dev_ci = {0};
    dev_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_ci.pNext = &f13;
    dev_ci.queueCreateInfoCount = 1;
    dev_ci.pQueueCreateInfos = &queue_ci;
    dev_ci.enabledExtensionCount = 1;
    dev_ci.ppEnabledExtensionNames = dev_exts;
    VkResult r = vkCreateDevice(s->physical_device, &dev_ci, NULL, &s->device);
    if (r != VK_SUCCESS) {
        printf("[VK] VkDevice creation error (%d)\n", r);
        return false;
    }
    vkGetDeviceQueue(s->device, s->graphics_family, 0, &s->graphics_queue);
    s->present_queue = s->graphics_queue;
    return true;
}

bool vk_init(VkState *s) {
    if (!vk_create_instance(s))
        return false;
    if (glfwCreateWindowSurface(s->instance, s->window, NULL, &s->surface) != VK_SUCCESS) {
        printf("[VK] Error creating the window surface\n");
        return false;
    }
    if (!vk_pick_device(s) || !vk_create_device(s))
        return false;
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
    if (vkCreatePipelineLayout(s->device, &pl_ci, NULL, &s->pipeline_layout) != VK_SUCCESS)
        return false;

    /*GPU-driven scene: descriptor layouts, compute pipelines, pool.*/
    if (!vk_create_gpd_init(s)) {
        printf("[VK] Compute pipeline initialization failed\n");
        return false;
    }

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(s->window, &fb_w, &fb_h);
    if (!vk_create_swapchain(s, (uint32_t)(fb_w > 0 ? fb_w : 1), (uint32_t)(fb_h > 0 ? fb_h : 1)) ||
        !vk_create_pipelines(s))
        return false;

    VkCommandPoolCreateInfo cp_ci = {0};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = s->graphics_family;
    if (vkCreateCommandPool(s->device, &cp_ci, NULL, &s->command_pool) != VK_SUCCESS)
        return false;
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
            vkCreateFence(s->device, &fen_ci, NULL, &s->fences[i]) != VK_SUCCESS)
            return false;
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
            VkBuffer bufs[] = {f->vertex_buffer, f->gpd_map_inst, f->gpd_dyn_inst,
                               f->gpd_oriented, f->gpd_map_visible, f->gpd_dyn_visible,
                               f->gpd_verts, f->gpd_indirect, f->gpd_counts};
            VkDeviceMemory mems[] = {f->vertex_memory, f->gpd_map_mem, f->gpd_dyn_mem,
                                     f->gpd_ori_mem, f->gpd_mvis_mem, f->gpd_dvis_mem,
                                     f->gpd_vert_mem, f->gpd_ind_mem, f->gpd_cnt_mem};
            for (size_t k = 0; k < sizeof(bufs) / sizeof(bufs[0]); k++)
                if (bufs[k])
                    vkDestroyBuffer(s->device, bufs[k], NULL);
            /* freeing mapped memory unmaps it */
            for (size_t k = 0; k < sizeof(mems) / sizeof(mems[0]); k++)
                if (mems[k])
                    vkFreeMemory(s->device, mems[k], NULL);
            memset(f, 0, sizeof(*f));
            if (s->fences[i])
                vkDestroyFence(s->device, s->fences[i], NULL);
            if (s->sem_image[i])
                vkDestroySemaphore(s->device, s->sem_image[i], NULL);
            s->fences[i] = VK_NULL_HANDLE;
            s->sem_image[i] = VK_NULL_HANDLE;
        }
        if (s->command_pool)
            vkDestroyCommandPool(s->device, s->command_pool, NULL);

        /*Compute resources (the descriptor sets die with the pool).*/
        if (s->gpd_desc_pool) vkDestroyDescriptorPool(s->device, s->gpd_desc_pool, NULL);
        VkDescriptorSetLayout dsl[] = {s->gpd_ds_layout_cull, s->gpd_ds_layout_exp,
                                       s->gpd_ds_layout_exp_ori, s->gpd_ds_layout_final,
                                       s->gpd_scene_ds_layout};
        for (size_t k = 0; k < sizeof(dsl) / sizeof(dsl[0]); k++)
            if (dsl[k])
                vkDestroyDescriptorSetLayout(s->device, dsl[k], NULL);
        VkPipeline pipes[] = {s->gpd_pipe_cull_map, s->gpd_pipe_cull_dyn, s->gpd_pipe_exp_map,
                              s->gpd_pipe_exp_dyn,  s->gpd_pipe_exp_ori,  s->gpd_pipe_final};
        for (size_t k = 0; k < sizeof(pipes) / sizeof(pipes[0]); k++)
            if (pipes[k])
                vkDestroyPipeline(s->device, pipes[k], NULL);
        VkPipelineLayout pls[] = {s->gpd_pl_cull, s->gpd_pl_exp, s->gpd_pl_exp_ori,
                                  s->gpd_pl_final, s->gpd_scene_layout, s->pipeline_layout};
        for (size_t k = 0; k < sizeof(pls) / sizeof(pls[0]); k++)
            if (pls[k])
                vkDestroyPipelineLayout(s->device, pls[k], NULL);
        vk_destroy_pipelines(s);
        vk_destroy_swapchain_resources(s);
        if (s->swapchain)
            vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        vkDestroyDevice(s->device, NULL);
    }
    if (s->surface)
        vkDestroySurfaceKHR(s->instance, s->surface, NULL);
    if (s->instance)
        vkDestroyInstance(s->instance, NULL);
    GLFWwindow *window = s->window;
    memset(s, 0, sizeof(*s));
    s->window = window;
}
