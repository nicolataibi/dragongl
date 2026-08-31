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
        if (!f) {
            printf("Shader opening error: Both '%s' and '%s' failed.\n", path, alt_path);
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

/*Create the render pass (depends on the swapchain format).*/
static VkRenderPass vk_create_render_pass(VkState *s, VkFormat color_fmt) {
    VkAttachmentDescription att[2];
    VkAttachmentReference att_ref;
    VkAttachmentReference depth_ref;
    VkSubpassDescription subpass;
    VkRenderPassCreateInfo rp_ci;
    VkSubpassDependency dep;
    VkRenderPass rp = VK_NULL_HANDLE;

    memset(att, 0, sizeof(att));
    att[0].format = color_fmt;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&att_ref, 0, sizeof(att_ref));
    att_ref.attachment = 0;
    att_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&depth_ref, 0, sizeof(depth_ref));
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &att_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    memset(&rp_ci, 0, sizeof(rp_ci));
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 2;
    rp_ci.pAttachments = att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;
    vkCreateRenderPass(s->device, &rp_ci, NULL, &rp);
    return rp;
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
    gp_ci.renderPass = s->render_pass;
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
    gp_ci.renderPass          = s->render_pass;
    gp_ci.subpass             = 0;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp_ci, NULL, &s->pipeline_hud) != VK_SUCCESS) {
        printf("HUD pipeline creation error\n");
        return false;
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
    VkFramebufferCreateInfo fb_ci;
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

    s->render_pass = vk_create_render_pass(s, s->swapchain_format);
    if (!vk_create_pipelines(s)) {
        return false;
    }

    s->framebuffers = malloc(sizeof(VkFramebuffer) * s->image_count);
    for (i = 0; i < s->image_count; i++) {
        memset(&fb_ci, 0, sizeof(fb_ci));
        VkImageView attachments[] = { s->image_views[i], s->depth_image_view };
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = s->render_pass;
        fb_ci.attachmentCount = 2;
        fb_ci.pAttachments = attachments;
        fb_ci.width = s->swapchain_extent.width;
        fb_ci.height = s->swapchain_extent.height;
        fb_ci.layers = 1;
        vkCreateFramebuffer(s->device, &fb_ci, NULL, &s->framebuffers[i]);
    }

    s->current_image = 0;
    return true;
}

/*Destroys resources dependent on window size:
 * framebuffers, pipeline, render pass, image views, swapchain and depth.
 * Device, instance, layout, command pool and vertex buffer remain intact.*/
static void vk_destroy_swapchain(VkState *s) {
    uint32_t i;
    if (s->framebuffers) {
        for (i = 0; i < s->image_count; i++) {
            vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL);
        }
        free(s->framebuffers);
        s->framebuffers = NULL;
    }
    if (s->pipeline) {
        vkDestroyPipeline(s->device, s->pipeline, NULL);
        s->pipeline = VK_NULL_HANDLE;
    }
    if (s->pipeline_hud) {
        vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
        s->pipeline_hud = VK_NULL_HANDLE;
    }
    if (s->render_pass) {
        vkDestroyRenderPass(s->device, s->render_pass, NULL);
        s->render_pass = VK_NULL_HANDLE;
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
    app_info.apiVersion = VK_API_VERSION_1_0;

    glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (i = 0; i < ext_count; i++) exts[i] = glfw_exts[i];

    memset(&inst_ci, 0, sizeof(inst_ci));
    inst_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_ci.pApplicationInfo = &app_info;
    inst_ci.enabledExtensionCount = ext_count;
    inst_ci.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&inst_ci, NULL, &s->instance) != VK_SUCCESS) {
        printf("Error creating VkInstance\n");
        return false;
    }

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
    s->graphics_family = 0;
    for (i = 0; i < qf_count; i++) {
        if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            s->graphics_family = i;
            break;
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

    memset(&dev_ci, 0, sizeof(dev_ci));
    dev_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_ci.queueCreateInfoCount = 1;
    dev_ci.pQueueCreateInfos = &queue_ci;
    dev_ci.enabledExtensionCount = 1;
    dev_ci.ppEnabledExtensionNames = dev_exts;
    dev_ci.pEnabledFeatures = &dev_features;
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
    push_const.size = sizeof(float) * 17;

    memset(&pl_ci, 0, sizeof(pl_ci));
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push_const;
    vkCreatePipelineLayout(s->device, &pl_ci, NULL, &s->pipeline_layout);

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
    cb_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(s->device, &cb_ai, &s->command_buffer);

    memset(&sem_ci, 0, sizeof(sem_ci));
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_image);
    vkCreateSemaphore(s->device, &sem_ci, NULL, &s->sem_render);
    memset(&fen_ci, 0, sizeof(fen_ci));
    fen_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fen_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(s->device, &fen_ci, NULL, &s->fence_flight);

    /*3D scene vertices: 201x201 tiles * 36 vertices/cube + 36 for the player + NPC*/
    uint32_t scene_verts = 201 * 201 * 36 + 36 + MAX_NPCS * 36 + MAX_PARTICLES * 36;
    /*2D HUD Vertices:
     * - Minimap: MINIMAP_BUF_SIZE^2 * 6 ≈ 81*81*6 = 39366
     * - Text (5x7 font): ~200 chars * 35 pixels/char * 6 vertices = 42000
     * - Quad boss trophies/equip: ~100 * 6 = 600
     * - Total rounded with margin*/
    uint32_t hud_verts = 39366 + 42000 + 2000;
    s->max_vertices = scene_verts + hud_verts;
    memset(&buf_ci, 0, sizeof(buf_ci));
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = sizeof(VkVertex) * s->max_vertices;
    buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(s->device, &buf_ci, NULL, &s->vertex_buffer);
    vkGetBufferMemoryRequirements(s->device, s->vertex_buffer, &mem_req);
    memset(&mem_ai, 0, sizeof(mem_ai));
    mem_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_ai.allocationSize = mem_req.size;
    mem_ai.memoryTypeIndex = vk_find_memory_type(s, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(s->device, &mem_ai, NULL, &s->vertex_memory);
    vkBindBufferMemory(s->device, s->vertex_buffer, s->vertex_memory, 0);

    printf("[VK] Vulkan initialization complete.\n");
    return true;
}

void vk_cleanup(VkState *s) {
    uint32_t i;
    vkDeviceWaitIdle(s->device);
    vkDestroyBuffer(s->device, s->vertex_buffer, NULL);
    vkFreeMemory(s->device, s->vertex_memory, NULL);
    vkDestroyFence(s->device, s->fence_flight, NULL);
    vkDestroySemaphore(s->device, s->sem_render, NULL);
    vkDestroySemaphore(s->device, s->sem_image, NULL);
    vkDestroyCommandPool(s->device, s->command_pool, NULL);
    if (s->framebuffers) {
        for (i = 0; i < s->image_count; i++) {
            vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL);
        }
        free(s->framebuffers);
        s->framebuffers = NULL;
    }
    if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
    if (s->pipeline_hud) vkDestroyPipeline(s->device, s->pipeline_hud, NULL);
    if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
    if (s->render_pass) vkDestroyRenderPass(s->device, s->render_pass, NULL);
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
