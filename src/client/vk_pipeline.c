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
 * vk_pipeline.c - the HUD pipeline and the scene pipeline
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

/*The 2D HUD pipeline (host vertex buffer, alpha blending, no depth test)
 *and the scene pipeline. They depend on the colour and depth formats
 *only — viewport and scissor are dynamic — so a resize keeps them.*/
bool vk_create_pipelines(VkState *s) {
    VkShaderModule vert_mod = vk_load_shader(s, "shader.vert.spv");
    VkShaderModule frag_mod = vk_load_shader(s, "shader.frag.spv");
    if (vert_mod == VK_NULL_HANDLE || frag_mod == VK_NULL_HANDLE) {
        if (vert_mod) {
            vkDestroyShaderModule(s->device, vert_mod, NULL);
        }
        if (frag_mod) {
            vkDestroyShaderModule(s->device, frag_mod, NULL);
        }
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

void vk_destroy_pipelines(VkState *s) {
    if (s->pipeline_hud) {
        vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
        s->pipeline_hud = VK_NULL_HANDLE;
    }
    if (s->gpd_scene_pipeline) {
        vkDestroyPipeline(s->device, s->gpd_scene_pipeline, NULL);
        s->gpd_scene_pipeline = VK_NULL_HANDLE;
    }
}
