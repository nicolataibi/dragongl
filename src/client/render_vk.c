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
 * render_vk.c - The window and the render loop: one frame after the
 * other until the window is closed or the session ends, a new swapchain
 * when the window changes, a new renderer when the device is lost.
 */

#include "render_vk.h"

#include "client_input.h"
#include "client_state.h"
#include "histogram.h"
#include "strnum.h"
#include "vk_frame.h"
#include "vk_scene.h"

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>

/*Times the renderer is rebuilt after a lost device before giving up: a
 * driver reset is survivable, a GPU that keeps failing is not.*/
#define MAX_RENDERER_RESTARTS 3

static VkState vk_state;

static void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
    vk_state.swapchain_dirty = true;
    client_input_resized(window, width, height);
}

/*Initial window size: DRAGONGL_WINDOW=<width>x<height> if set, otherwise
 *80% of the primary monitor's work area (the part not taken by panels),
 *never below 800x600. The window is resizable; the swapchain follows.*/
static void initial_window_size(int *w, int *h) {
    *w = 1280;
    *h = 800;
    const char *env = getenv("DRAGONGL_WINDOW");
    int wh[2] = {0, 0};
    int got = env ? scan_ints(env, 'x', 2, wh) : 0;
    int ew = wh[0], eh = wh[1];
    if (got == 2 && ew >= 320 && eh >= 240 && ew <= 16384 && eh <= 16384) {
        *w = ew;
        *h = eh;
        return;
    }
    GLFWmonitor *mon = glfwGetPrimaryMonitor();
    if (mon) {
        int x = 0, y = 0, aw = 0, ah = 0;
        glfwGetMonitorWorkarea(mon, &x, &y, &aw, &ah);
        if (aw > 0 && ah > 0) {
            *w = aw * 4 / 5;
            *h = ah * 4 / 5;
        }
    }
    if (*w < 800) {
        *w = 800;
    }
    if (*h < 600) {
        *h = 600;
    }
}

/*Destroy and rebuild the swapchain for the current framebuffer size. A
 * minimized window (0x0) is waited out here: no swapchain can have a zero
 * extent. Returns false only when the swapchain cannot be rebuilt.*/
static bool recreate_swapchain(VkState *s) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(s->window, &fb_w, &fb_h);
    while ((fb_w == 0 || fb_h == 0) && !glfwWindowShouldClose(s->window) && g_running) {
        glfwWaitEventsTimeout(0.1);
        glfwGetFramebufferSize(s->window, &fb_w, &fb_h);
    }
    if (fb_w == 0 || fb_h == 0) {
        return true; /* closing */
    }
    s->swapchain_dirty = false;
    return vk_recreate_swapchain(s, (uint32_t)fb_w, (uint32_t)fb_h);
}

/*The device or the surface is gone (driver reset, GPU removed, a crash of
 * the driver): everything Vulkan is destroyed and created again on the
 * same window. The game state is untouched, the session goes on.*/
static bool restart_renderer(VkState *s) {
    static int restarts;
    if (++restarts > MAX_RENDERER_RESTARTS) {
        printf("[VK] The device was lost %d times: closing.\n", restarts);
        return false;
    }
    printf("[VK] Device or surface lost: restarting the renderer (%d of %d).\n", restarts,
           MAX_RENDERER_RESTARTS);
    vk_cleanup(s);
    vk_scene_reset();
    if (!vk_init(s)) {
        printf("[VK] The renderer cannot be restarted.\n");
        return false;
    }
    printf("[VK] Renderer restarted.\n");
    return true;
}

/*DRAGONGL_TEST_DEVICE_LOST=<n>: the device is reported lost at frame n
 * (tests/client: the restart cannot be provoked on a real driver).*/
static unsigned long test_device_lost_frame(void) {
    const char *env = getenv("DRAGONGL_TEST_DEVICE_LOST");
    int n = env ? str_to_int(env) : 0;
    return n > 0 ? (unsigned long)n : 0;
}

static void print_frame_times(const MetricHistogram *frames) {
    MetricSummary sum;
    histogram_summary(frames, &sum);
    if (sum.count > 0) {
        printf("[VK] Frames: %llu, frame time p50 %.2f p99 %.2f max %.2f ms\n",
               (unsigned long long)sum.count, sum.p50_ms, sum.p99_ms, sum.max_ms);
    }
}

/*Frames until the window closes, the session ends, max_frames (if not 0)
 * are drawn or the renderer cannot go on.*/
static void render_loop(VkState *s, unsigned long max_frames) {
    static MetricHistogram frames;
    unsigned long lost_at = test_device_lost_frame(), drawn = 0;
    uint64_t last_us = metrics_now_us();
    float frame_ms = 0.0f; /*smoothed, for the HUD*/
    while (!glfwWindowShouldClose(s->window) && g_running) {
        glfwPollEvents();
        if (lost_at != 0 && drawn == lost_at) {
            printf("[VK] Test: device lost at frame %lu.\n", drawn);
            lost_at = 0;
            s->device_lost = true;
        }
        if (s->device_lost && !restart_renderer(s)) {
            break;
        }
        if (s->swapchain_dirty && !recreate_swapchain(s)) {
            break; /* swapchain not recoverable */
        }
        uint64_t now_us = metrics_now_us();
        uint64_t frame_us = now_us - last_us;
        last_us = now_us;
        float dt = (float)frame_us / 1e6f;
        if (drawn > 0) { /*the first interval is not a frame*/
            histogram_add(&frames, frame_us, 0);
            float ms = (float)frame_us / 1000.0f;
            frame_ms = frame_ms > 0.0f ? frame_ms * 0.9f + ms * 0.1f : ms;
        }
        vk_draw_frame(s, dt > 0.1f ? 0.1f : dt, glfwGetTime(), frame_ms);
        drawn++;
        if (max_frames != 0 && drawn >= max_frames) {
            break;
        }
    }
    print_frame_times(&frames);
}

void render_vk_start(const ClientConfig *cfg, unsigned long max_frames) {
    if (!glfwInit()) {
        printf("GLFW initialization error.\n");
        return;
    }
    if (!glfwVulkanSupported()) {
        printf("[VK] No Vulkan loader or driver found (glfwVulkanSupported).\n");
        glfwTerminate();
        return;
    }

    int win_w = 0, win_h = 0;
    initial_window_size(&win_w, &win_h);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    vk_state.window = glfwCreateWindow(win_w, win_h, "Dragon GL", NULL, NULL);
    if (!vk_state.window) {
        printf("Error creating GLFW window.\n");
        glfwTerminate();
        return;
    }
    glfwSetWindowSizeLimits(vk_state.window, 640, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    client_input_install(vk_state.window, cfg);
    glfwSetFramebufferSizeCallback(vk_state.window, framebuffer_size_callback);

    if (vk_init(&vk_state)) {
        render_loop(&vk_state, max_frames);
    }
    vk_cleanup(&vk_state);
    vk_scene_reset();
    glfwDestroyWindow(vk_state.window);
    glfwTerminate();
}
