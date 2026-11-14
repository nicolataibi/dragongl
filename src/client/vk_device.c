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
 * vk_device.c - the Vulkan instance, the choice of the GPU and the logical device
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

bool vk_create_instance(VkState *s) {
    uint32_t loader_version = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enum_version =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (enum_version) {
        enum_version(&loader_version);
    }
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
    if (wanted && wanted[0] &&
        vkEnumerateInstanceLayerProperties(&avail_count, NULL) == VK_SUCCESS && avail_count > 0) {
        avail = malloc(sizeof(*avail) * avail_count);
    }
    if (avail && vkEnumerateInstanceLayerProperties(&avail_count, avail) == VK_SUCCESS) {
        char *copy = strdup(wanted);
        char *sp = NULL;
        for (char *tok = copy ? strtok_r(copy, ":", &sp) : NULL; tok;
             tok = strtok_r(NULL, ":", &sp)) {
            bool have = false;
            for (uint32_t li = 0; li < avail_count && !have; li++) {
                if (strcmp(tok, avail[li].layerName) == 0) {
                    have = true;
                    if (layer_count < 16) {
                        layers[layer_count++] = avail[li].layerName;
                    }
                }
            }
            if (!have) {
                printf("[VK] Warning: layer '%s' (VK_INSTANCE_LAYERS) not available - skipping.\n",
                       tok);
            }
        }
        free(copy);
    }
    if (layer_count > 0) {
        printf("[VK] Enabling %u layer(s)\n", layer_count);
    }

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
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "software";
    default:
        return "other";
    }
}

/*Why `dev` cannot run the game (NULL if it can); *family receives a
 *queue family with graphics, compute and presentation to the surface.*/
static const char *device_unsuitable(VkState *s, VkPhysicalDevice dev, uint32_t *family) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        return "Vulkan 1.3 not supported";
    }

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, NULL, &n, NULL);
    VkExtensionProperties *exts = n ? malloc(sizeof(*exts) * n) : NULL;
    bool swapchain = false;
    if (exts) {
        vkEnumerateDeviceExtensionProperties(dev, NULL, &n, exts);
        for (uint32_t i = 0; i < n; i++) {
            if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                swapchain = true;
            }
        }
        free(exts);
    }
    if (!swapchain) {
        return "no VK_KHR_swapchain";
    }

    VkPhysicalDeviceVulkan13Features f13 = {0};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {0};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(dev, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) {
        return "no dynamicRendering/synchronization2";
    }

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
    if (!found) {
        return "no graphics+compute queue that can present to the window";
    }

    uint32_t formats = 0, modes = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, s->surface, &formats, NULL);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, s->surface, &modes, NULL);
    if (formats == 0 || modes == 0) {
        return "cannot present to the window";
    }
    return NULL;
}

/*Device-local memory, in MiB (tie-breaker between similar devices).*/
static uint64_t device_local_mib(VkPhysicalDevice dev) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    uint64_t best = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mp.memoryHeaps[i].size > best) {
            best = mp.memoryHeaps[i].size;
        }
    }
    return best >> 20;
}

/*Pick the GPU: every suitable device is scored (discrete > integrated >
 *virtual > software, then device-local memory). DRAGONGL_GPU=<index>
 *or a part of the device name selects one explicitly.*/
bool vk_pick_device(VkState *s) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(s->instance, &n, NULL) != VK_SUCCESS || n == 0) {
        printf("[VK] No Vulkan device found.\n");
        return false;
    }
    VkPhysicalDevice *devs = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * n);
    if (!devs) {
        return false;
    }
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
        printf("[VK] GPU %u: %s (%s)%s%s\n", i, props.deviceName,
               device_type_name(props.deviceType), why ? " - unusable: " : "", why ? why : "");
        if (why) {
            continue;
        }
        int64_t score;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 4000000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 3000000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 2000000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score = 1000000;
            break;
        default:
            score = 0;
            break;
        }
        uint64_t mib = device_local_mib(devs[i]);
        score += (int64_t)(mib > 999999 ? 999999 : mib);
        if (want && want[0]) {
            bool match =
                want_is_index ? (want_index == (long)i) : (strstr(props.deviceName, want) != NULL);
            if (match) {
                score += 100000000;
            }
        }
        if (score > best_score) {
            best_score = score;
            s->physical_device = devs[i];
            s->graphics_family = family;
        }
    }
    free((void *)devs);
    if (best_score < 0) {
        printf("[VK] No usable Vulkan 1.3 device.\n");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s->physical_device, &props);
    printf("[VK] Using %s (Vulkan %u.%u, queue family %u)\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           s->graphics_family);
    if (want && want[0] && best_score < 100000000) {
        printf("[VK] Warning: DRAGONGL_GPU=%s matches no usable device.\n", want);
    }
    return true;
}

bool vk_create_device(VkState *s) {
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
