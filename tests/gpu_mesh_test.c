/*
 * DRAGON GL - Headless Vulkan test for the GPU-driven mesh generation.
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
 *
 * Verifies the GPD compute passes against a CPU reference, in TWO
 * ISOLATED STAGES (GPU culling and GPU mesh allocation are developed
 * and debugged separately — this test reports them separately too):
 *
 *   Stage CULL   : gpd_cull_map + gpd_cull_dyn. The visible-instance
 *                  index lists are read back and compared (as sets)
 *                  with the CPU cull reference.
 *   Stage EXPAND : gpd_expand_map + gpd_expand_dyn +
 *                  gpd_expand_oriented + gpd_final, consuming the index
 *                  lists from stage 1 (like the client does). The
 *                  generated vertex SSBO is read back: every expected
 *                  primitive block must be found intact inside it
 *                  (boxes/pyramids bit-exact — this file is compiled
 *                  with -ffp-contract=off to match the non-FMA shader;
 *                  oriented boxes within 1e-4 because the mat3*vec3
 *                  add order is driver-defined), and the indirect
 *                  command must carry the exact total.
 *
 * Usage:  test_gpu_mesh [spv_dir]      (default: ./build/shaders)
 * Exit:   0 = pass, 1 = fail, 77 = skip (no Vulkan ICD / missing .spv).
 *
 * The reference geometry intentionally mirrors push_box / push_pyramid /
 * push_box_oriented in src/client/render_vk.c — if those change, update
 * both the GLSL tables (shaders/gpd_mesh.glsl) and this file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

/* ------------------------------------------------------------------ */
/* CPU mirrors of the GPU-side structures (keep in sync with           */
/* src/client/render_vk.h and shaders/gpd_mesh.glsl).                  */
/* ------------------------------------------------------------------ */

#define VKD_VERTEX_STRIDE 48

typedef struct {
    float pos[3];
    float pad0;
    float color[4];
    float normal[3];
    float pad1;
} GpdVertexOut; /* 48 B == GLSL GpdVertex */

typedef struct {
    float pos[3];
    float type;
    float scale[3];
    float pad;
    float color[3];
    float alpha;
} GpdInstance; /* 48 B == GpdInstance */

typedef struct {
    float pos[3];
    float type;
    float scale[3];
    float pad;
    float color[3];
    float alpha;
    float orient[9];
    float pad3[3];
} GpdOriented; /* 64 B == GpdOriented */

typedef struct {
    float cull_radius;
    float player_x;
    float player_z;
    float time;
    uint32_t count;
    uint32_t capacity;
} GpdPush; /* 24 B == GpdPC */

typedef struct {
    uint32_t verts;
    uint32_t map_vis;
    uint32_t dyn_vis;
} GpdCounts; /* 12 B in a 16 B buffer */

enum { TYPE_BOX = 0, TYPE_PYRAMID = 1, TYPE_PARTICLE = 2 };

/* ------------------------------------------------------------------ */
/* Vulkan scaffolding                                                  */
/* ------------------------------------------------------------------ */

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static VkCommandPool g_pool;
static VkDescriptorPool g_desc_pool;
static VkDescriptorSetLayout g_ds_cull, g_ds_exp, g_ds_exp_ori, g_ds_final;
static VkPipelineLayout g_pl_cull, g_pl_exp, g_pl_exp_ori, g_pl_final;
static VkPipeline g_pipe_cull_map, g_pipe_cull_dyn,
                  g_pipe_exp_map, g_pipe_exp_dyn,
                  g_pipe_exp_ori, g_pipe_final;
static char g_spv_dir[512] = "./build/shaders";

static bool check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "  [VK] %s failed: %d\n", what, (int)r);
        return false;
    }
    return true;
}

static bool create_ds_layout(uint32_t nb, const uint32_t *bind,
                             VkDescriptorSetLayout *out) {
    VkDescriptorSetLayoutBinding *b = malloc(sizeof(*b) * nb);
    VkDescriptorSetLayoutCreateInfo ci = {0};
    for (uint32_t i = 0; i < nb; i++) {
        b[i].binding = bind[i];
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = nb;
    ci.pBindings = b;
    bool ok = check(vkCreateDescriptorSetLayout(g_dev, &ci, NULL, out), "ds layout");
    free(b);
    return ok;
}

static bool make_compute_pipeline(const char *spv, VkPipelineLayout layout,
                                  VkPipeline *out) {
    FILE *f = fopen(spv, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)sz);
    if (fread(code, 1, (size_t)sz, f) != (size_t)sz) {
        free(code);
        fclose(f);
        return false;
    }
    fclose(f);
    VkShaderModule mod;
    VkShaderModuleCreateInfo sm = {0};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = (size_t)sz;
    sm.pCode = code;
    bool ok = check(vkCreateShaderModule(g_dev, &sm, NULL, &mod), "shader module");
    free(code);
    if (!ok) return false;

    VkPipelineShaderStageCreateInfo st = {0};
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    st.module = mod;
    st.pName = "main";
    VkComputePipelineCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = st;
    ci.layout = layout;
    ok = check(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, out),
               "compute pipeline");
    vkDestroyShaderModule(g_dev, mod, NULL);
    return ok;
}

static bool create_gpd_pipelines(void) {
    static const uint32_t b3[3] = {0, 1, 2};
    static const uint32_t b4[4] = {0, 1, 2, 3};
    static const uint32_t b3ori[3] = {0, 2, 3};
    static const uint32_t b2[2] = {0, 1};

    if (!create_ds_layout(3, b3, &g_ds_cull)) return false;
    if (!create_ds_layout(4, b4, &g_ds_exp)) return false;
    if (!create_ds_layout(3, b3ori, &g_ds_exp_ori)) return false;
    if (!create_ds_layout(2, b2, &g_ds_final)) return false;

    VkDescriptorSetLayout sls[4] = {
        g_ds_cull, g_ds_exp, g_ds_exp_ori, g_ds_final
    };
    VkPipelineLayout *pls[4] = {
        &g_pl_cull, &g_pl_exp, &g_pl_exp_ori, &g_pl_final
    };
    for (int p = 0; p < 4; p++) {
        VkPushConstantRange pc = {0};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.size = sizeof(GpdPush);
        VkPipelineLayoutCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &sls[p];
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        if (!check(vkCreatePipelineLayout(g_dev, &ci, NULL, pls[p]), "pl"))
            return false;
    }

    char path[560];
    struct { const char *spv; VkPipelineLayout pl; VkPipeline *out; } pipes[6] = {
        { "gpd_cull_map.comp.spv",         g_pl_cull,    &g_pipe_cull_map },
        { "gpd_cull_dyn.comp.spv",         g_pl_cull,    &g_pipe_cull_dyn },
        { "gpd_expand_map.comp.spv",       g_pl_exp,     &g_pipe_exp_map },
        { "gpd_expand_dyn.comp.spv",       g_pl_exp,     &g_pipe_exp_dyn },
        { "gpd_expand_oriented.comp.spv",  g_pl_exp_ori, &g_pipe_exp_ori },
        { "gpd_final.comp.spv",            g_pl_final,   &g_pipe_final }
    };
    for (int p = 0; p < 6; p++) {
        snprintf(path, sizeof(path), "%s/%s", g_spv_dir, pipes[p].spv);
        if (!make_compute_pipeline(path, pipes[p].pl, pipes[p].out)) return false;
    }

    VkDescriptorPoolSize ps = {0};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 24;
    VkDescriptorPoolCreateInfo dpc = {0};
    dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpc.maxSets = 8;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &ps;
    return check(vkCreateDescriptorPool(g_dev, &dpc, NULL, &g_desc_pool), "desc pool");
}

static bool init_vulkan(void) {
    uint32_t pdn = 0, i;
    VkPhysicalDevice *pds;
    uint32_t fam = UINT32_MAX;
    int dev = -1;

    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dragongl_gpu_mesh_test";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) return false;
    if (vkEnumeratePhysicalDevices(g_inst, &pdn, NULL) != VK_SUCCESS || pdn == 0)
        return false;
    pds = malloc(sizeof(VkPhysicalDevice) * pdn);
    vkEnumeratePhysicalDevices(g_inst, &pdn, pds);
    /* Same selection rule as the client: first device with a
     * graphics+compute queue family, else compute-only. */
    for (i = 0; i < pdn && dev < 0; i++) {
        uint32_t qfn;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qfn, NULL);
        VkQueueFamilyProperties *qf = malloc(sizeof(*qf) * qfn);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qfn, qf);
        for (uint32_t k = 0; k < qfn; k++) {
            if ((qf[k].queueFlags & VK_QUEUE_COMPUTE_BIT) && dev < 0) {
                dev = (int)i;
                fam = k;
                if (qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
            }
        }
        free(qf);
    }
    if (dev < 0) {
        free(pds);
        return false;
    }
    g_phys = pds[dev];
    free(pds);

    VkDeviceQueueCreateInfo qc = {0};
    qc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qc.queueFamilyIndex = fam;
    qc.queueCount = 1;
    static const float prio = 1.0f;
    qc.pQueuePriorities = &prio;
    VkDeviceCreateInfo dc = {0};
    dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    if (vkCreateDevice(g_phys, &dc, NULL, &g_dev) != VK_SUCCESS) return false;
    vkGetDeviceQueue(g_dev, fam, 0, &g_queue);

    VkCommandPoolCreateInfo pc = {0};
    pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.queueFamilyIndex = fam;
    if (!check(vkCreateCommandPool(g_dev, &pc, NULL, &g_pool), "command pool"))
        return false;
    return create_gpd_pipelines();
}

/* All test buffers are host-visible: readback-first debugging. */
static bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer *buf, VkDeviceMemory *mem, void **host) {
    VkBufferCreateInfo bc = {0};
    bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bc.size = size;
    bc.usage = usage;
    if (!check(vkCreateBuffer(g_dev, &bc, NULL, buf), "buffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, *buf, &req);
    uint32_t type = 0, found = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            type = i;
            found = 1;
            break;
        }
    }
    if (!found) return false;
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (!check(vkAllocateMemory(g_dev, &ai, NULL, mem), "alloc")) return false;
    if (!check(vkBindBufferMemory(g_dev, *buf, *mem, 0), "bind")) return false;
    if (host && !check(vkMapMemory(g_dev, *mem, 0, req.size, 0, host), "map"))
        return false;
    return true;
}

static bool submit_and_wait(VkCommandBuffer cb) {
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkFence fence;
    VkFenceCreateInfo fc = {0};
    fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(g_dev, &fc, NULL, &fence) != VK_SUCCESS) return false;
    if (vkQueueSubmit(g_queue, 1, &si, fence) != VK_SUCCESS) {
        vkDestroyFence(g_dev, fence, NULL);
        return false;
    }
    bool ok = false;
    for (int tries = 0; tries < 5; tries++) {
        if (vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 2000000000ULL) == VK_SUCCESS) {
            ok = true;
            break;
        }
    }
    vkDestroyFence(g_dev, fence, NULL);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test data (CPU side; copied into the GPU buffers in main)           */
/* ------------------------------------------------------------------ */

#define N_MAP  200
#define N_DYN  12
#define N_ORI  2
#define N_VMAX 10000

static GpdInstance ref_map[N_MAP];
static GpdInstance ref_dyn[N_DYN];
static GpdOriented ref_ori[N_ORI];

/* GPU buffer aliases (persistent mapping, set in main). */
static GpdInstance *u_map_inst, *u_dyn_inst;
static GpdOriented *u_ori;
static uint32_t *u_map_vis, *u_dyn_vis;
static GpdVertexOut *u_verts;
static uint32_t *u_indirect;
static GpdCounts *u_counts;
static VkBuffer b_map_inst, b_dyn_inst, b_ori, b_map_vis, b_dyn_vis,
                b_verts, b_indirect, b_counts;
static VkDeviceMemory m_map_inst, m_dyn_inst, m_ori, m_map_vis, m_dyn_vis,
                      m_verts, m_indirect, m_counts;
static VkDescriptorSet set_cull_map, set_cull_dyn, set_exp_map, set_exp_dyn,
                       set_exp_ori, set_final;

static const float PLAYER_X = 0.0f, PLAYER_Z = 0.0f, CULL_R = 5.0f, T = 1.0f;

static void rot_y(float a, float m[9]) {
    float c = cosf(a), s = sinf(a);
    m[0] = c;  m[1] = 0;  m[2] = -s;
    m[3] = 0;  m[4] = 1;  m[5] = 0;
    m[6] = s;  m[7] = 0;  m[8] = c;
}
static void rot_x(float a, float m[9]) {
    float c = cosf(a), s = sinf(a);
    m[0] = 1;  m[1] = 0;  m[2] = 0;
    m[3] = 0;  m[4] = c;  m[5] = s;
    m[6] = 0;  m[7] = -s; m[8] = c;
}

static void fill_test_data(void) {
    int i;
    memset(ref_map, 0, sizeof(ref_map));
    for (i = 0; i < N_MAP; i++) {
        ref_map[i].pos[0] = (float)(i % 16) - 8.0f;
        ref_map[i].pos[1] = 0.5f;
        ref_map[i].pos[2] = (float)((i / 16) % 13) - 6.0f;
        ref_map[i].type = (float)TYPE_BOX;
        ref_map[i].alpha = 1.0f;
        if (i % 3 == 0) {
            ref_map[i].scale[0] = 0.5f; ref_map[i].scale[1] = 1.5f; ref_map[i].scale[2] = 0.5f;
            ref_map[i].color[0] = 0.6f; ref_map[i].color[1] = 0.6f; ref_map[i].color[2] = 0.6f;
        } else if (i % 3 == 1) {
            ref_map[i].scale[0] = 0.5f; ref_map[i].scale[1] = 0.1f; ref_map[i].scale[2] = 0.5f;
            ref_map[i].color[0] = 0.3f; ref_map[i].color[1] = 0.3f; ref_map[i].color[2] = 0.3f;
        } else {
            ref_map[i].scale[0] = 0.5f; ref_map[i].scale[1] = 0.05f; ref_map[i].scale[2] = 0.5f;
            ref_map[i].color[0] = 0.1f; ref_map[i].color[1] = 0.4f; ref_map[i].color[2] = 0.8f;
        }
    }

    memset(ref_dyn, 0, sizeof(ref_dyn));
    /* 0: player pyramid (in range) */
    ref_dyn[0].pos[0] = 0.4f; ref_dyn[0].pos[1] = 0.4f; ref_dyn[0].pos[2] = 0.2f;
    ref_dyn[0].type = (float)TYPE_PYRAMID;
    ref_dyn[0].scale[0] = 0.35f; ref_dyn[0].scale[1] = 0.5f; ref_dyn[0].scale[2] = 0.35f;
    ref_dyn[0].color[0] = 0.2f; ref_dyn[0].color[1] = 0.8f; ref_dyn[0].color[2] = 0.2f;
    ref_dyn[0].alpha = 1.0f;
    /* 1: merchant box (in range) */
    ref_dyn[1].pos[0] = 2.0f; ref_dyn[1].pos[1] = 0.4f; ref_dyn[1].pos[2] = 1.0f;
    ref_dyn[1].type = (float)TYPE_BOX;
    ref_dyn[1].scale[0] = 0.3f; ref_dyn[1].scale[1] = 0.4f; ref_dyn[1].scale[2] = 0.3f;
    ref_dyn[1].color[0] = 1.0f; ref_dyn[1].color[1] = 0.8f; ref_dyn[1].color[2] = 0.0f;
    ref_dyn[1].alpha = 1.0f;
    /* 2: far box -> culled */
    ref_dyn[2].pos[0] = 50.0f; ref_dyn[2].pos[1] = 0.4f; ref_dyn[2].pos[2] = 0.0f;
    ref_dyn[2].type = (float)TYPE_BOX;
    ref_dyn[2].scale[0] = 0.3f; ref_dyn[2].scale[1] = 0.4f; ref_dyn[2].scale[2] = 0.3f;
    ref_dyn[2].color[0] = 1.0f; ref_dyn[2].color[1] = 0.3f; ref_dyn[2].color[2] = 0.3f;
    ref_dyn[2].alpha = 1.0f;
    /* 3: particle (never culled, in range) */
    ref_dyn[3].pos[0] = 0.1f; ref_dyn[3].pos[1] = -0.3f; ref_dyn[3].pos[2] = 0.2f;
    ref_dyn[3].type = (float)TYPE_PARTICLE;
    ref_dyn[3].scale[0] = 0.1f; ref_dyn[3].scale[1] = 0.1f; ref_dyn[3].scale[2] = 0.1f;
    ref_dyn[3].color[0] = 0.8f; ref_dyn[3].color[1] = 0.4f; ref_dyn[3].color[2] = 0.1f;
    ref_dyn[3].alpha = 1.0f;
    /* 4: FAR particle -> NOT culled (particles are never culled) */
    ref_dyn[4].pos[0] = 99.0f; ref_dyn[4].pos[1] = -0.5f; ref_dyn[4].pos[2] = 99.0f;
    ref_dyn[4].type = (float)TYPE_PARTICLE;
    ref_dyn[4].scale[0] = 0.1f; ref_dyn[4].scale[1] = 0.1f; ref_dyn[4].scale[2] = 0.1f;
    ref_dyn[4].color[0] = 0.5f; ref_dyn[4].color[1] = 0.5f; ref_dyn[4].color[2] = 0.5f;
    ref_dyn[4].alpha = 1.0f;
    /* 5: far pyramid -> culled */
    ref_dyn[5].pos[0] = 30.0f; ref_dyn[5].pos[1] = 0.4f; ref_dyn[5].pos[2] = -30.0f;
    ref_dyn[5].type = (float)TYPE_PYRAMID;
    ref_dyn[5].scale[0] = 0.35f; ref_dyn[5].scale[1] = 0.5f; ref_dyn[5].scale[2] = 0.35f;
    ref_dyn[5].color[0] = 0.2f; ref_dyn[5].color[1] = 0.8f; ref_dyn[5].color[2] = 0.2f;
    ref_dyn[5].alpha = 1.0f;
    /* 6: ON the cull circle (dist == r) -> NOT culled (strict >) */
    ref_dyn[6].pos[0] = 5.0f; ref_dyn[6].pos[1] = 0.4f; ref_dyn[6].pos[2] = 0.0f;
    ref_dyn[6].type = (float)TYPE_BOX;
    ref_dyn[6].scale[0] = 0.3f; ref_dyn[6].scale[1] = 0.4f; ref_dyn[6].scale[2] = 0.3f;
    ref_dyn[6].color[0] = 0.9f; ref_dyn[6].color[1] = 0.9f; ref_dyn[6].color[2] = 0.9f;
    ref_dyn[6].alpha = 1.0f;
    /* 7: just OUTSIDE the cull circle -> culled */
    ref_dyn[7].pos[0] = 5.1f; ref_dyn[7].pos[1] = 0.4f; ref_dyn[7].pos[2] = 0.0f;
    ref_dyn[7].type = (float)TYPE_BOX;
    ref_dyn[7].scale[0] = 0.3f; ref_dyn[7].scale[1] = 0.4f; ref_dyn[7].scale[2] = 0.3f;
    ref_dyn[7].color[0] = 0.9f; ref_dyn[7].color[1] = 0.5f; ref_dyn[7].color[2] = 0.1f;
    ref_dyn[7].alpha = 1.0f;
    /* 8..11: in-range boxes with distinct colors */
    for (i = 8; i < N_DYN; i++) {
        ref_dyn[i].pos[0] = (float)(i - 8) * 0.7f;
        ref_dyn[i].pos[1] = 0.4f;
        ref_dyn[i].pos[2] = -1.5f * (float)(i % 3);
        ref_dyn[i].type = (float)TYPE_BOX;
        ref_dyn[i].scale[0] = 0.2f; ref_dyn[i].scale[1] = 0.3f; ref_dyn[i].scale[2] = 0.2f;
        ref_dyn[i].color[0] = 0.1f * (float)i;
        ref_dyn[i].color[1] = 0.2f * (float)(i % 5);
        ref_dyn[i].color[2] = 0.3f * (float)((i + 1) % 7);
        ref_dyn[i].alpha = 1.0f;
    }

    memset(ref_ori, 0, sizeof(ref_ori));
    ref_ori[0].pos[0] = 1.2f; ref_ori[0].pos[1] = 0.8f; ref_ori[0].pos[2] = -0.7f;
    ref_ori[0].type = (float)TYPE_BOX;
    ref_ori[0].scale[0] = 0.45f; ref_ori[0].scale[1] = 0.1f; ref_ori[0].scale[2] = 0.35f;
    ref_ori[0].color[0] = 0.9f; ref_ori[0].color[1] = 0.75f; ref_ori[0].color[2] = 0.3f;
    ref_ori[0].alpha = 1.0f;
    rot_y(0.6f, ref_ori[0].orient);
    ref_ori[1].pos[0] = -2.1f; ref_ori[1].pos[1] = 1.1f; ref_ori[1].pos[2] = 2.4f;
    ref_ori[1].type = (float)TYPE_BOX;
    ref_ori[1].scale[0] = 0.3f; ref_ori[1].scale[1] = 0.08f; ref_ori[1].scale[2] = 0.25f;
    ref_ori[1].color[0] = 0.7f; ref_ori[1].color[1] = 0.3f; ref_ori[1].color[2] = 0.9f;
    ref_ori[1].alpha = 1.0f;
    rot_x(0.3f, ref_ori[1].orient);
}

/* ------------------------------------------------------------------ */
/* CPU reference: culling (exact copy of the shader comparison)        */
/* ------------------------------------------------------------------ */

static bool cpu_cull_visible(const GpdInstance *it) {
    if (it->type == (float)TYPE_PARTICLE) return true;
    float dx = it->pos[0] - PLAYER_X;
    float dz = it->pos[2] - PLAYER_Z;
    return !(dx * dx + dz * dz > CULL_R * CULL_R);
}

/* ------------------------------------------------------------------ */
/* CPU reference: expansion — mirrors the GLSL tables (gpd_mesh.glsl)  */
/* in corner order, so a box/pyramid block is reproducible bit-exact   */
/* (this file is compiled with -ffp-contract=off; glslc does not       */
/* FMA-contract either).                                               */
/* ------------------------------------------------------------------ */

static const int BOX_SIGNS[36][3] = {
    {-1,1,-1},{ 1,1,-1},{-1,1, 1},{ 1,1,-1},{ 1,1, 1},{-1,1, 1},
    {-1,-1,-1},{-1,-1, 1},{ 1,-1,-1},{ 1,-1,-1},{-1,-1, 1},{ 1,-1, 1},
    {-1,-1, 1},{ 1,-1, 1},{-1, 1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    {-1,-1,-1},{-1, 1,-1},{ 1,-1,-1},{ 1,-1,-1},{-1, 1,-1},{ 1, 1,-1},
    {-1,-1,-1},{-1,-1, 1},{-1, 1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},
    { 1,-1,-1},{ 1, 1,-1},{ 1,-1, 1},{ 1,-1,-1},{ 1, 1, 1},{ 1,-1, 1}
};
static const float BOX_FACE_N[6][3] = {
    {0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{-1,0,0},{1,0,0}
};
static const int PYM_SIGNS[18][3] = {
    {0,1,0},{-1,-1,1},{ 1,-1,1},
    {0,1,0},{ 1,-1,1},{ 1,-1,-1},
    {0,1,0},{ 1,-1,-1},{-1,-1,-1},
    {0,1,0},{-1,-1,-1},{-1,-1,1},
    {-1,-1,-1},{ 1,-1,-1},{-1,-1,1},
    { 1,-1,-1},{ 1,-1,1},{-1,-1,1}
};
static const float PYM_N[18][3] = {
    {0,0.5,1},{0,0.5,1},{0,0.5,1},
    {1,0.5,0},{1,0.5,0},{1,0.5,0},
    {0,0.5,-1},{0,0.5,-1},{0,0.5,-1},
    {-1,0.5,0},{-1,0.5,0},{-1,0.5,0},
    {0,-1,0},{0,-1,0},{0,-1,0},
    {0,-1,0},{0,-1,0},{0,-1,0}
};

static void ref_box(GpdVertexOut *v, const GpdInstance *it) {
    for (int i = 0; i < 36; i++) {
        GpdVertexOut *o = &v[i];
        o->pos[0] = it->pos[0] + (float)BOX_SIGNS[i][0] * it->scale[0];
        o->pos[1] = it->pos[1] + (float)BOX_SIGNS[i][1] * it->scale[1];
        o->pos[2] = it->pos[2] + (float)BOX_SIGNS[i][2] * it->scale[2];
        o->color[0] = it->color[0]; o->color[1] = it->color[1];
        o->color[2] = it->color[2]; o->color[3] = it->alpha;
        o->normal[0] = BOX_FACE_N[i / 6][0];
        o->normal[1] = BOX_FACE_N[i / 6][1];
        o->normal[2] = BOX_FACE_N[i / 6][2];
    }
}

static void ref_pyramid(GpdVertexOut *v, const GpdInstance *it) {
    for (int i = 0; i < 18; i++) {
        GpdVertexOut *o = &v[i];
        o->pos[0] = it->pos[0] + (float)PYM_SIGNS[i][0] * it->scale[0];
        o->pos[1] = it->pos[1] + (float)PYM_SIGNS[i][1] * it->scale[1];
        o->pos[2] = it->pos[2] + (float)PYM_SIGNS[i][2] * it->scale[2];
        o->color[0] = it->color[0]; o->color[1] = it->color[1];
        o->color[2] = it->color[2]; o->color[3] = it->alpha;
        o->normal[0] = PYM_N[i][0];
        o->normal[1] = PYM_N[i][1];
        o->normal[2] = PYM_N[i][2];
    }
}

static void ref_oriented(GpdVertexOut *v, const GpdOriented *it) {
    for (int k = 0; k < 36; k++) {
        GpdVertexOut *o = &v[k];
        float lx = (float)BOX_SIGNS[k][0] * it->scale[0];
        float ly = (float)BOX_SIGNS[k][1] * it->scale[1];
        float lz = (float)BOX_SIGNS[k][2] * it->scale[2];
        /* Same chain as the CPU push_box_oriented(): o0*l + o3*l + o6*l.
         * The GLSL mat3*vec3 may group the adds differently -> the test
         * compares oriented blocks with a 1e-4 tolerance. */
        o->pos[0] = it->pos[0] + it->orient[0] * lx + it->orient[3] * ly + it->orient[6] * lz;
        o->pos[1] = it->pos[1] + it->orient[1] * lx + it->orient[4] * ly + it->orient[7] * lz;
        o->pos[2] = it->pos[2] + it->orient[2] * lx + it->orient[5] * ly + it->orient[8] * lz;
        o->color[0] = it->color[0]; o->color[1] = it->color[1];
        o->color[2] = it->color[2]; o->color[3] = it->alpha;
        float fn[3] = { BOX_FACE_N[k / 6][0], BOX_FACE_N[k / 6][1], BOX_FACE_N[k / 6][2] };
        for (int a = 0; a < 3; a++)
            o->normal[a] = it->orient[a] * fn[0] + it->orient[3 + a] * fn[1] + it->orient[6 + a] * fn[2];
    }
}

/* ------------------------------------------------------------------ */
/* Stage 1: CULL                                                       */
/* ------------------------------------------------------------------ */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static bool stage_cull(void) {
    printf("  [CULL] dispatching cull_map(%d) + cull_dyn(%d)...\n", N_MAP, N_DYN);

    memset(u_counts, 0, sizeof(*u_counts));
    memset(u_map_vis, 0, sizeof(uint32_t) * N_MAP);
    memset(u_dyn_vis, 0, sizeof(uint32_t) * N_DYN);

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev, &cai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &cbi);

    /* CPU (host) writes -> compute */
    VkBufferMemoryBarrier bmb[3] = {0};
    bmb[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb[0].srcAccessMask = 0;
    bmb[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bmb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb[0].buffer = b_map_inst; bmb[0].offset = 0; bmb[0].size = VK_WHOLE_SIZE;
    bmb[1] = bmb[0]; bmb[1].buffer = b_dyn_inst;
    bmb[2] = bmb[0]; bmb[2].buffer = b_counts;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 3, bmb, 0, NULL);

    GpdPush pc_map = { CULL_R, PLAYER_X, PLAYER_Z, T, N_MAP, N_VMAX };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_cull_map);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_cull, 0,
                            1, &set_cull_map, 0, NULL);
    vkCmdPushConstants(cb, g_pl_cull, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pc_map), &pc_map);
    vkCmdDispatch(cb, (N_MAP + 255) / 256, 1, 1);

    GpdPush pc_dyn = { CULL_R, PLAYER_X, PLAYER_Z, T, N_DYN, N_VMAX };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_cull_dyn);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_cull, 0,
                            1, &set_cull_dyn, 0, NULL);
    vkCmdPushConstants(cb, g_pl_cull, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pc_dyn), &pc_dyn);
    vkCmdDispatch(cb, (N_DYN + 255) / 256, 1, 1);

    vkEndCommandBuffer(cb);
    if (!submit_and_wait(cb)) {
        printf("  [CULL] FAIL: submit/wait\n");
        return false;
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cb);

    /* ---- compare against the CPU cull reference ---- */
    uint32_t exp_map[N_MAP], exp_dyn[N_DYN], ne = 0, nd = 0;
    for (int i = 0; i < N_MAP; i++)
        if (cpu_cull_visible(&ref_map[i])) exp_map[ne++] = (uint32_t)i;
    for (int i = 0; i < N_DYN; i++)
        if (cpu_cull_visible(&ref_dyn[i])) exp_dyn[nd++] = (uint32_t)i;

    bool ok = true;
    if (u_counts->map_vis != ne) {
        printf("  [CULL] FAIL: map visible count gpu=%u expected=%u\n",
               u_counts->map_vis, ne);
        ok = false;
    }
    if (u_counts->dyn_vis != nd) {
        printf("  [CULL] FAIL: dyn visible count gpu=%u expected=%u\n",
               u_counts->dyn_vis, nd);
        ok = false;
    }
    if (u_counts->verts != 0) {
        printf("  [CULL] FAIL: verts counter not zero after cull stage: %u\n",
               u_counts->verts);
        ok = false;
    }
    if (ok) {
        /* compare as sets (order in the index list is not guaranteed) */
        uint32_t got_map[N_MAP], got_dyn[N_DYN];
        memcpy(got_map, u_map_vis, ne * sizeof(uint32_t));
        memcpy(got_dyn, u_dyn_vis, nd * sizeof(uint32_t));
        qsort(got_map, ne, sizeof(uint32_t), cmp_u32);
        qsort(exp_map, ne, sizeof(uint32_t), cmp_u32);
        for (uint32_t i = 0; i < ne; i++) {
            if (got_map[i] != exp_map[i]) {
                printf("  [CULL] FAIL: map index set differs at sorted pos %u: gpu=%u expected=%u\n",
                       i, got_map[i], exp_map[i]);
                ok = false;
                break;
            }
        }
        qsort(got_dyn, nd, sizeof(uint32_t), cmp_u32);
        qsort(exp_dyn, nd, sizeof(uint32_t), cmp_u32);
        for (uint32_t i = 0; i < nd; i++) {
            if (got_dyn[i] != exp_dyn[i]) {
                printf("  [CULL] FAIL: dyn index set differs at sorted pos %u: gpu=%u expected=%u\n",
                       i, got_dyn[i], exp_dyn[i]);
                ok = false;
                break;
            }
        }
    }
    if (ok)
        printf("  [CULL] PASS: map %u/%u visible, dyn %u/%u visible (far "
               "particle kept, on-circle box kept, off-circle box dropped)\n",
               u_counts->map_vis, N_MAP, u_counts->dyn_vis, N_DYN);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Stage 2: EXPAND (mesh allocation)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    GpdVertexOut verts[36];
    uint32_t n;
    bool ori;      /* originally an oriented block (tolerance match) */
    bool matched;  /* consumed from the GPU buffer */
} ExpBlock;

#define MAX_BLOCKS 300
static ExpBlock g_blocks[MAX_BLOCKS];
static int g_nblocks = 0;

static void expect_box(const GpdInstance *it) {
    ExpBlock *b = &g_blocks[g_nblocks++];
    ref_box(b->verts, it);
    b->n = 36; b->ori = false; b->matched = false;
}
static void expect_pyramid(const GpdInstance *it) {
    ExpBlock *b = &g_blocks[g_nblocks++];
    ref_pyramid(b->verts, it);
    b->n = 18; b->ori = false; b->matched = false;
}
static void expect_oriented(const GpdOriented *it) {
    ExpBlock *b = &g_blocks[g_nblocks++];
    ref_oriented(b->verts, it);
    b->n = 36; b->ori = true; b->matched = false;
}

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t vhash(const GpdVertexOut *v) {
    uint64_t h = 1469598103934665603ULL;
    h ^= fbits(v->pos[0]); h *= 1099511628211ULL;
    h ^= fbits(v->pos[1]); h *= 1099511628211ULL;
    h ^= fbits(v->pos[2]); h *= 1099511628211ULL;
    h ^= (uint64_t)fbits(v->color[0]) << 1; h *= 1099511628211ULL;
    return h;
}

static bool verts_equal(const GpdVertexOut *a, const GpdVertexOut *b, bool tol) {
    for (int f = 0; f < 3; f++) {
        if (a->pos[f] != b->pos[f] &&
            (!tol || fabsf(a->pos[f] - b->pos[f]) > 1e-4f)) return false;
        if (a->color[f] != b->color[f]) return false;
        if (a->normal[f] != b->normal[f] &&
            (!tol || fabsf(a->normal[f] - b->normal[f]) > 1e-4f)) return false;
    }
    return a->color[3] == b->color[3];
}

static bool stage_expand(void) {
    printf("  [EXPAND] dispatching expand_map + expand_dyn + expand_oriented + final...\n");

    /* Fresh GPU state for the expand stage: the index lists from stage 1
     * are still in the buffers (exactly like the client); only the verts
     * counter restarts at 0 (the CPU resets it every frame). */
    u_counts->verts = 0;
    memset(u_verts, 0, sizeof(GpdVertexOut) * N_VMAX);
    memset(u_indirect, 0, sizeof(uint32_t) * 4);
    fprintf(stderr, "  [DBG] before expand: counts={verts=0 map_vis=%u dyn_vis=%u} first vis idx: %u %u\n",
            u_counts->map_vis, u_counts->dyn_vis,
            u_map_vis[0], u_map_vis[1]);

    /* CPU reference expectation (same cull decisions as stage 1). */
    g_nblocks = 0;
    for (int i = 0; i < N_MAP; i++)
        if (cpu_cull_visible(&ref_map[i])) expect_box(&ref_map[i]);
    for (int i = 0; i < N_DYN; i++) {
        if (!cpu_cull_visible(&ref_dyn[i])) continue;
        if (ref_dyn[i].type == (float)TYPE_PYRAMID) expect_pyramid(&ref_dyn[i]);
        else expect_box(&ref_dyn[i]);
    }
    for (int i = 0; i < N_ORI; i++) expect_oriented(&ref_ori[i]);
    uint32_t expected_total = 0;
    for (int i = 0; i < g_nblocks; i++) expected_total += g_blocks[i].n;

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev, &cai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &cbi);

    GpdPush pc = { CULL_R, PLAYER_X, PLAYER_Z, T, 0, N_VMAX };

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_exp_map);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_exp, 0,
                            1, &set_exp_map, 0, NULL);
    vkCmdPushConstants(cb, g_pl_exp, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (N_MAP + 255) / 256, 1, 1);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_exp_dyn);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_exp, 0,
                            1, &set_exp_dyn, 0, NULL);
    vkCmdPushConstants(cb, g_pl_exp, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (N_DYN + 255) / 256, 1, 1);

    GpdPush pc_ori = { CULL_R, PLAYER_X, PLAYER_Z, T, N_ORI, N_VMAX };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_exp_ori);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_exp_ori, 0,
                            1, &set_exp_ori, 0, NULL);
    vkCmdPushConstants(cb, g_pl_exp_ori, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_ori), &pc_ori);
    vkCmdDispatch(cb, (N_ORI + 255) / 256, 1, 1);

    VkMemoryBarrier mb = {0};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_final);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_final, 0,
                            1, &set_final, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);

    vkEndCommandBuffer(cb);
    if (!submit_and_wait(cb)) {
        printf("  [EXPAND] FAIL: submit/wait\n");
        return false;
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cb);

    fprintf(stderr, "  [DBG] after expand: counts={verts=%u map_vis=%u dyn_vis=%u} indirect={%u,%u}\n",
            u_counts->verts, u_counts->map_vis, u_counts->dyn_vis,
            u_indirect[0], u_indirect[1]);
    {
        int nz = 0;
        for (uint32_t o = 0; o < N_VMAX && nz < 5; o++)
            if (u_verts[o].pos[0] != 0.0f || u_verts[o].color[0] != 0.0f) nz++;
        fprintf(stderr, "  [DBG] non-zero vertices found: %d\n", nz);
    }

    bool ok = true;
    if (u_counts->verts != expected_total) {
        printf("  [EXPAND] FAIL: vertex counter gpu=%u expected=%u\n",
               u_counts->verts, expected_total);
        ok = false;
    }
    if (u_indirect[0] != expected_total || u_indirect[1] != 1 ||
        u_indirect[2] != 0 || u_indirect[3] != 0) {
        printf("  [EXPAND] FAIL: indirect command {%u,%u,%u,%u} expected {%u,1,0,0}\n",
               u_indirect[0], u_indirect[1], u_indirect[2], u_indirect[3],
               expected_total);
        ok = false;
    }

    /* Every expected block must appear intact in the vertex SSBO. The
     * vertex ORDER is not deterministic (parallel reservations), so
     * exact blocks are matched by hash of their first vertex, and
     * oriented blocks (driver-defined add order) by tolerance scan. */
    if (u_counts->verts == expected_total) {
        bool used[N_VMAX];
        memset(used, 0, sizeof(used));

        /* hash table: first-vertex hash -> block index (open addressing) */
        #define SHT 4096
        long sh[SHT];
        for (int i = 0; i < SHT; i++) sh[i] = -1;
        for (int i = 0; i < g_nblocks; i++) {
            if (g_blocks[i].ori) continue;
            long slot = (long)(vhash(&g_blocks[i].verts[0]) % SHT);
            while (sh[slot] >= 0) slot = (slot + 1) % SHT;
            sh[slot] = i;
        }
        for (uint32_t o = 0; o + 1 <= u_counts->verts; o++) {
            if (used[o]) continue;
            long slot = (long)(vhash(&u_verts[o]) % SHT);
            int bi = -1;
            while (sh[slot] >= 0) {
                int j = (int)sh[slot];
                if (!g_blocks[j].ori && !g_blocks[j].matched &&
                    g_blocks[j].n + o <= u_counts->verts) {
                    bool region_free = true;
                    for (uint32_t k = 0; k < g_blocks[j].n; k++)
                        if (used[o + k]) { region_free = false; break; }
                    if (region_free &&
                        verts_equal(&u_verts[o], &g_blocks[j].verts[0], false)) {
                        bool full = true;
                        for (uint32_t k = 1; k < g_blocks[j].n; k++)
                            if (!verts_equal(&u_verts[o + k], &g_blocks[j].verts[k], false)) {
                                full = false;
                                break;
                            }
                        if (full) { bi = j; break; }
                    }
                }
                slot = (slot + 1) % SHT;
            }
            if (bi >= 0) {
                for (uint32_t k = 0; k < g_blocks[bi].n; k++) used[o + k] = true;
                g_blocks[bi].matched = true;
                o += g_blocks[bi].n - 1;
            }
        }
        /* Tolerance pass: the (few) oriented blocks. */
        for (int i = 0; i < g_nblocks; i++) {
            if (!g_blocks[i].ori || g_blocks[i].matched) continue;
            bool found = false;
            for (uint32_t o = 0; o + g_blocks[i].n <= u_counts->verts; o++) {
                bool region_free = true;
                for (uint32_t k = 0; k < g_blocks[i].n; k++)
                    if (used[o + k]) { region_free = false; break; }
                if (!region_free) continue;
                bool match = true;
                for (uint32_t k = 0; k < g_blocks[i].n; k++)
                    if (!verts_equal(&u_verts[o + k], &g_blocks[i].verts[k], true)) {
                        match = false;
                        break;
                    }
                if (match) {
                    for (uint32_t k = 0; k < g_blocks[i].n; k++) used[o + k] = true;
                    found = true;
                    break;
                }
            }
            if (found) {
                g_blocks[i].matched = true;
            } else {
                printf("  [EXPAND] FAIL: oriented block (pos %g,%g,%g) not found intact in the vertex SSBO\n",
                       g_blocks[i].verts[0].pos[0], g_blocks[i].verts[0].pos[1],
                       g_blocks[i].verts[0].pos[2]);
                ok = false;
            }
        }
        /* Any block never consumed = corrupted/missing geometry. */
        for (int i = 0; i < g_nblocks; i++) {
            if (!g_blocks[i].matched) {
                printf("  [EXPAND] FAIL: block %d (n=%u, pos %g,%g,%g) missing or corrupted\n",
                       i, g_blocks[i].n,
                       g_blocks[i].verts[0].pos[0], g_blocks[i].verts[0].pos[1],
                       g_blocks[i].verts[0].pos[2]);
                ok = false;
            }
        }
        #undef SHT
    }
    if (ok)
        printf("  [EXPAND] PASS: %d blocks, %u vertices (box/pyramid bit-exact, "
               "oriented within 1e-4), indirect command correct\n",
               g_nblocks, u_counts->verts);
    return ok;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_spv_dir, sizeof(g_spv_dir), "%s", argv[1]);

    printf("GPU mesh generation test (GPD): cull / expand stages\n");
    printf("  spv dir: %s\n", g_spv_dir);

    char probe[560];
    snprintf(probe, sizeof(probe), "%s/gpd_final.comp.spv", g_spv_dir);
    if (access(probe, R_OK) != 0) {
        printf("  SKIP: %s not found (build the main project first)\n", probe);
        return 77;
    }
    if (!init_vulkan()) {
        printf("  SKIP: no Vulkan device with a compute queue\n");
        return 77;
    }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        printf("  device: %s\n", pp.deviceName);
    }

    fill_test_data();

    bool ok = true;
    ok &= make_buffer(sizeof(GpdInstance) * N_MAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_map_inst, &m_map_inst, (void **)&u_map_inst);
    ok &= make_buffer(sizeof(GpdInstance) * N_DYN, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_dyn_inst, &m_dyn_inst, (void **)&u_dyn_inst);
    ok &= make_buffer(sizeof(GpdOriented) * N_ORI, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_ori, &m_ori, (void **)&u_ori);
    ok &= make_buffer(sizeof(uint32_t) * N_MAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_map_vis, &m_map_vis, (void **)&u_map_vis);
    ok &= make_buffer(sizeof(uint32_t) * N_DYN, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_dyn_vis, &m_dyn_vis, (void **)&u_dyn_vis);
    ok &= make_buffer(sizeof(GpdVertexOut) * N_VMAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_verts, &m_verts, (void **)&u_verts);
    ok &= make_buffer(16,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      &b_indirect, &m_indirect, (void **)&u_indirect);
    ok &= make_buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      &b_counts, &m_counts, (void **)&u_counts);
    if (!ok) {
        printf("FAIL: buffer setup\n");
        return 1;
    }
    memcpy(u_map_inst, ref_map, sizeof(ref_map));
    memcpy(u_dyn_inst, ref_dyn, sizeof(ref_dyn));
    memcpy(u_ori, ref_ori, sizeof(ref_ori));

    /* Descriptor sets. */
    {
        VkDescriptorSetLayout layouts[6] = {
            g_ds_cull, g_ds_cull, g_ds_exp, g_ds_exp, g_ds_exp_ori, g_ds_final
        };
        VkDescriptorSet sets[6];
        VkDescriptorSetAllocateInfo sai = {0};
        sai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sai.descriptorPool = g_desc_pool;
        sai.descriptorSetCount = 6;
        sai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(g_dev, &sai, sets) != VK_SUCCESS) {
            printf("FAIL: descriptor set allocation\n");
            return 1;
        }
        set_cull_map = sets[0]; set_cull_dyn = sets[1];
        set_exp_map = sets[2];  set_exp_dyn = sets[3];
        set_exp_ori = sets[4];  set_final = sets[5];

        struct { uint32_t set, binding; VkBuffer buf; } table[19] = {
            {0, 0, b_map_inst},  {0, 1, b_map_vis}, {0, 2, b_counts},
            {1, 0, b_dyn_inst},  {1, 1, b_dyn_vis}, {1, 2, b_counts},
            {2, 0, b_map_inst},  {2, 1, b_map_vis}, {2, 2, b_counts}, {2, 3, b_verts},
            {3, 0, b_dyn_inst},  {3, 1, b_dyn_vis}, {3, 2, b_counts}, {3, 3, b_verts},
            {4, 0, b_ori},       {4, 2, b_counts},  {4, 3, b_verts},
            {5, 0, b_indirect},  {5, 1, b_counts}
        };
        VkDescriptorBufferInfo bi[19] = {0};
        VkWriteDescriptorSet w[19] = {0}; /* dstArrayElement = 0 for all */
        for (int k = 0; k < 19; k++) {
            bi[k].buffer = table[k].buf;
            bi[k].offset = 0;
            bi[k].range = VK_WHOLE_SIZE;
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = sets[table[k].set];
            w[k].dstBinding = table[k].binding;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(g_dev, 19, w, 0, NULL);
    }

    bool ok_cull = stage_cull();
    bool ok_exp = stage_expand();

    printf("\nRESULT: cull=%s expand=%s\n",
           ok_cull ? "PASS" : "FAIL", ok_exp ? "PASS" : "FAIL");
    return (ok_cull && ok_exp) ? 0 : 1;
}
