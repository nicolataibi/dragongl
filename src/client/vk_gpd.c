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
 * vk_gpd.c - the GPU-driven scene: compute pipelines, per-frame buffers and descriptor sets
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

#define GPD_SETS        7  /*descriptor sets of one frame slot*/
#define GPD_DESCRIPTORS 20 /*storage buffers bound in those sets*/

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
static bool gpd_create_compute_pipeline(VkState *s, const char *spv_name, VkPipelineLayout layout,
                                        VkPipeline *out) {
    VkShaderModule mod = vk_load_shader(s, spv_name);
    if (mod == VK_NULL_HANDLE) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stage = {0};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo cp_ci = {0};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage = stage;
    cp_ci.layout = layout;
    bool ok =
        (vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &cp_ci, NULL, out) == VK_SUCCESS);
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

/*A set layout of storage buffers at the given binding numbers (they need
 * not be contiguous), visible to one shader stage.*/
static bool gpd_set_layout(VkState *s, const uint32_t *binding_numbers, uint32_t count,
                           VkShaderStageFlags stage, VkDescriptorSetLayout *out) {
    VkDescriptorSetLayoutBinding bindings[4] = {0};
    for (uint32_t i = 0; i < count; i++) {
        gpd_binding(&bindings[i], binding_numbers[i]);
        bindings[i].stageFlags = stage;
    }
    VkDescriptorSetLayoutCreateInfo dsl_ci = {0};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = count;
    dsl_ci.pBindings = bindings;
    return vkCreateDescriptorSetLayout(s->device, &dsl_ci, NULL, out) == VK_SUCCESS;
}

/*The five set layouts (one per binding shape - see the table in
 * vk_create_gpd_frame_buffers for which set fills which buffer).*/
static bool gpd_create_set_layouts(VkState *s) {
    static const uint32_t cull[] = {0, 1, 2};
    static const uint32_t expand[] = {0, 1, 2, 3};
    /*expand_oriented has no index list: no binding 1.*/
    static const uint32_t expand_oriented[] = {0, 2, 3};
    static const uint32_t final[] = {0, 1};
    /*The scene reads the compute-generated vertices.*/
    static const uint32_t scene[] = {0};
    const VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
    return gpd_set_layout(s, cull, 3, comp, &s->gpd_ds_layout_cull) &&
           gpd_set_layout(s, expand, 4, comp, &s->gpd_ds_layout_exp) &&
           gpd_set_layout(s, expand_oriented, 3, comp, &s->gpd_ds_layout_exp_ori) &&
           gpd_set_layout(s, final, 2, comp, &s->gpd_ds_layout_final) &&
           gpd_set_layout(s, scene, 1, VK_SHADER_STAGE_VERTEX_BIT, &s->gpd_scene_ds_layout);
}

/*A pipeline layout: one set and one range of push constants.*/
static bool gpd_pipeline_layout(VkState *s, const VkDescriptorSetLayout *set,
                                VkShaderStageFlags stages, uint32_t push_size,
                                VkPipelineLayout *out) {
    VkPushConstantRange pc_range = {0};
    pc_range.stageFlags = stages;
    pc_range.size = push_size;
    VkPipelineLayoutCreateInfo pl_ci = {0};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = set;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pc_range;
    return vkCreatePipelineLayout(s->device, &pl_ci, NULL, out) == VK_SUCCESS;
}

/*Each compute set layout with the 24 B compute push constants (GpdPush
 * == GpdPC in gpd_mesh.glsl); the scene with the push constants of the
 * HUD pipeline (mvp, visionRadius, playerX, playerZ, time).*/
static bool gpd_create_pipeline_layouts(VkState *s) {
    const VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
    const uint32_t push = (uint32_t)sizeof(GpdPush);
    return gpd_pipeline_layout(s, &s->gpd_ds_layout_cull, comp, push, &s->gpd_pl_cull) &&
           gpd_pipeline_layout(s, &s->gpd_ds_layout_exp, comp, push, &s->gpd_pl_exp) &&
           gpd_pipeline_layout(s, &s->gpd_ds_layout_exp_ori, comp, push, &s->gpd_pl_exp_ori) &&
           gpd_pipeline_layout(s, &s->gpd_ds_layout_final, comp, push, &s->gpd_pl_final) &&
           gpd_pipeline_layout(s, &s->gpd_scene_ds_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               (uint32_t)(sizeof(float) * 20), &s->gpd_scene_layout);
}

/*Persistent GPD resources (set layouts, pipeline layouts, pool, the 6
 * compute pipelines).*/
bool vk_create_gpd_init(VkState *s) {
    if (!gpd_create_set_layouts(s) || !gpd_create_pipeline_layouts(s)) {
        return false;
    }

    /*7 sets per in-flight frame (one per pipeline use; see
     * vk_create_gpd_frame_buffers). 3+3+4+4+3+2+1 = 20 descriptors.*/
    VkDescriptorPoolSize pool_size = {0};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = GPD_DESCRIPTORS * 4; /*up to 4 frames*/
    VkDescriptorPoolCreateInfo pool_ci = {0};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = GPD_SETS * 4;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(s->device, &pool_ci, NULL, &s->gpd_desc_pool) != VK_SUCCESS) {
        return false;
    }

    /*Cull passes (stage 1) then expand passes (stage 2) then final.*/
    struct {
        const char *spv;
        VkPipelineLayout pl;
        VkPipeline *out;
    } pipes[6] = {{"gpd_cull_map.comp.spv", s->gpd_pl_cull, &s->gpd_pipe_cull_map},
                  {"gpd_cull_dyn.comp.spv", s->gpd_pl_cull, &s->gpd_pipe_cull_dyn},
                  {"gpd_expand_map.comp.spv", s->gpd_pl_exp, &s->gpd_pipe_exp_map},
                  {"gpd_expand_dyn.comp.spv", s->gpd_pl_exp, &s->gpd_pipe_exp_dyn},
                  {"gpd_expand_oriented.comp.spv", s->gpd_pl_exp_ori, &s->gpd_pipe_exp_ori},
                  {"gpd_final.comp.spv", s->gpd_pl_final, &s->gpd_pipe_final}};
    for (int p = 0; p < 6; p++) {
        if (!gpd_create_compute_pipeline(s, pipes[p].spv, pipes[p].pl, pipes[p].out)) {
            return false;
        }
    }
    return true;
}

/*The scene pipeline: no vertex input — the vertex shader reads the
 * compute-generated vertices from the storage buffer (binding 0).*/
bool vk_create_gpd_scene_pipeline(VkState *s) {
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
    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) {
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

/*Per-slot GPD ring: 8 buffers.
 *
 *   buffer          filled by    read by
 *   gpd_map_inst    CPU (walk)   cull_map, expand_map
 *   gpd_dyn_inst    CPU (per fr) cull_dyn, expand_dyn
 *   gpd_oriented    CPU (per fr) expand_oriented
 *   gpd_map_visible GPU cull_map expand_map
 *   gpd_dyn_visible GPU cull_dyn expand_dyn
 *   gpd_verts       GPU expand   scene (drawIndirect)
 *   gpd_indirect    GPU final    vkCmdDrawIndirect
 *   gpd_counts      GPU fill +   cull/expand/final (GPU accumulate)
 *
 * The first three are host-visible and mapped; the others are
 * device-local: only the GPU writes and reads them.*/
static bool gpd_create_buffers(VkState *s, VkFrameResources *f) {
    const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const struct {
        VkDeviceSize size;
        VkBufferUsageFlags usage;
        VkBuffer *buf;
        VkDeviceMemory *mem;
        void **mapped; /*NULL: device-local*/
    } b[8] = {
        {(VkDeviceSize)VKD_MAP_INST_MAX * sizeof(GpdInstance), ssbo, &f->gpd_map_inst,
         &f->gpd_map_mem, &f->gpd_map_ptr},
        {(VkDeviceSize)VKD_DYN_INST_MAX * sizeof(GpdInstance), ssbo, &f->gpd_dyn_inst,
         &f->gpd_dyn_mem, &f->gpd_dyn_ptr},
        {(VkDeviceSize)VKD_ORIENTED_MAX * sizeof(GpdOriented), ssbo, &f->gpd_oriented,
         &f->gpd_ori_mem, &f->gpd_ori_ptr},
        /*Cull indices.*/
        {(VkDeviceSize)VKD_MAP_INST_MAX * sizeof(uint32_t), ssbo, &f->gpd_map_visible,
         &f->gpd_mvis_mem, NULL},
        {(VkDeviceSize)VKD_DYN_INST_MAX * sizeof(uint32_t), ssbo, &f->gpd_dyn_visible,
         &f->gpd_dvis_mem, NULL},
        /*VKD_VERTEX_STRIDE must match the GLSL GpdVertex layout in
         * gpd_mesh.glsl (vec3 pos; vec4 color; vec3 normal -> 48 B).*/
        {(VkDeviceSize)VKD_VERTEX_CAPACITY * VKD_VERTEX_STRIDE, ssbo, &f->gpd_verts,
         &f->gpd_vert_mem, NULL},
        /*One VkDrawIndirectCommand.*/
        {16, ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &f->gpd_indirect, &f->gpd_ind_mem, NULL},
        /*GpdCounts: 3 x uint32, padded to 16; reset with vkCmdFillBuffer
         * at the start of every frame.*/
        {16, ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f->gpd_counts, &f->gpd_cnt_mem, NULL},
    };
    for (int i = 0; i < 8; i++) {
        if (!vk_create_buffer(s, b[i].size, b[i].usage, b[i].mapped != NULL, b[i].buf, b[i].mem,
                              b[i].mapped)) {
            return false;
        }
    }
    return true;
}

/*The buffers of one frame slot and its 7 descriptor sets, one per
 * pipeline use (written once: the buffers never change after creation).*/
bool vk_create_gpd_frame_buffers(VkState *s, uint32_t i) {
    VkFrameResources *f = &s->frames[i];
    if (!gpd_create_buffers(s, f)) {
        return false;
    }

    VkDescriptorSetLayout layouts[GPD_SETS] = {
        s->gpd_ds_layout_cull,    /*cull_map: 0=map_inst 1=m_visible 2=counts*/
        s->gpd_ds_layout_cull,    /*cull_dyn: 0=dyn_inst 1=d_visible 2=counts*/
        s->gpd_ds_layout_exp,     /*exp_map:  0=map_inst 1=m_visible 2=counts 3=verts*/
        s->gpd_ds_layout_exp,     /*exp_dyn:  0=dyn_inst 1=d_visible 2=counts 3=verts*/
        s->gpd_ds_layout_exp_ori, /*exp_ori:  0=oriented 2=counts 3=verts*/
        s->gpd_ds_layout_final,   /*final:    0=indirect 1=counts*/
        s->gpd_scene_ds_layout,   /*scene:    0=verts*/
    };
    VkDescriptorSet sets[GPD_SETS];
    VkDescriptorSetAllocateInfo sai = {0};
    sai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sai.descriptorPool = s->gpd_desc_pool;
    sai.descriptorSetCount = GPD_SETS;
    sai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &sai, sets) != VK_SUCCESS) {
        return false;
    }
    f->gpd_desc_cull_map = sets[0];
    f->gpd_desc_cull_dyn = sets[1];
    f->gpd_desc_exp_map = sets[2];
    f->gpd_desc_exp_dyn = sets[3];
    f->gpd_desc_exp_ori = sets[4];
    f->gpd_desc_final = sets[5];
    f->gpd_desc_scene = sets[6];

    /*3+3 (cull) + 4+4 (expand) + 3 (exp_ori) + 2 (final) + 1 (scene).*/
    const struct {
        uint32_t set, binding;
        VkBuffer buf;
    } table[GPD_DESCRIPTORS] = {
        {0, 0, f->gpd_map_inst}, {0, 1, f->gpd_map_visible}, {0, 2, f->gpd_counts},
        {1, 0, f->gpd_dyn_inst}, {1, 1, f->gpd_dyn_visible}, {1, 2, f->gpd_counts},
        {2, 0, f->gpd_map_inst}, {2, 1, f->gpd_map_visible}, {2, 2, f->gpd_counts},
        {2, 3, f->gpd_verts},    {3, 0, f->gpd_dyn_inst},    {3, 1, f->gpd_dyn_visible},
        {3, 2, f->gpd_counts},   {3, 3, f->gpd_verts},       {4, 0, f->gpd_oriented},
        {4, 2, f->gpd_counts},   {4, 3, f->gpd_verts},       {5, 0, f->gpd_indirect},
        {5, 1, f->gpd_counts},   {6, 0, f->gpd_verts}};
    VkDescriptorBufferInfo bi[GPD_DESCRIPTORS] = {0};
    VkWriteDescriptorSet w[GPD_DESCRIPTORS] = {0};
    for (int k = 0; k < GPD_DESCRIPTORS; k++) {
        bi[k].buffer = table[k].buf;
        bi[k].range = VK_WHOLE_SIZE;
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = sets[table[k].set];
        w[k].dstBinding = table[k].binding;
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[k].pBufferInfo = &bi[k];
    }
    vkUpdateDescriptorSets(s->device, GPD_DESCRIPTORS, w, 0, NULL);
    return true;
}
