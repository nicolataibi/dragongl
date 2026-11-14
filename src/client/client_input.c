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
 * client_input.c - Keyboard and mouse of the window (client_input.h).
 */

#include "client_input.h"

#include "client_log.h"
#include "client_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*client_config.c names the keys by their GLFW codes without including
 * GLFW: these must agree.*/
_Static_assert(GLFW_KEY_SPACE == 32 && GLFW_KEY_SLASH == 47 && GLFW_KEY_EQUAL == 61 &&
                   GLFW_KEY_ESCAPE == 256 && GLFW_KEY_ENTER == 257 && GLFW_KEY_UP == 265 &&
                   GLFW_KEY_PAGE_UP == 266 && GLFW_KEY_END == 269 && GLFW_KEY_F1 == 290 &&
                   GLFW_KEY_F25 == 314 && GLFW_KEY_KP_0 == 320 && GLFW_KEY_KP_ENTER == 335,
               "the key codes of client_config.c are not those of GLFW");
_Static_assert(GLFW_MOD_SHIFT == KEYMOD_SHIFT && GLFW_MOD_CONTROL == KEYMOD_CTRL &&
                   GLFW_MOD_ALT == KEYMOD_ALT,
               "the modifier bits of client_config.h are not those of GLFW");

#define CAMERA_DIST_MIN  2.0f
#define CAMERA_DIST_MAX  200.0f
#define CAMERA_PITCH_MIN 10.0f
#define CAMERA_PITCH_MAX 89.0f
#define LOG_SCROLL_STEP  4
#define LOG_SCROLL_MAX   (CLIENT_LOG_LINES * 4)

static const ClientConfig *g_cfg;
static Camera g_camera = {0.0f, 45.0f, 25.0f};
static LineEdit g_line;
static ClientUi g_ui;
static float g_scale_auto = 1.0f;
static float g_scale_user; /*0: automatic*/
static int g_side_choice;  /*0: by the width of the window, 1: shown, -1: hidden*/
static float g_fb_width;
static bool g_swallow_char; /*the character of the key that opened the line*/
static double g_last_x, g_last_y;
static bool g_mouse_held;

const Camera *client_camera(void) {
    return &g_camera;
}

const ClientUi *client_ui(void) {
    return &g_ui;
}

const ClientConfig *client_input_config(void) {
    return g_cfg;
}

/*The HUD scale in use, and whether the side panels fit: they are hidden
 * by default on a window narrower than 1000 virtual pixels.*/
static void apply_scale(void) {
    g_ui.scale = g_scale_user > 0.0f ? g_scale_user : g_scale_auto;
    bool roomy = g_fb_width / g_ui.scale >= 1000.0f;
    g_ui.show_side = g_side_choice != 0 ? g_side_choice > 0 : roomy;
}

void client_input_resized(GLFWwindow *window, int framebuffer_width, int framebuffer_height) {
    float cx = 1.0f, cy = 1.0f;
    g_fb_width = (float)framebuffer_width;
    glfwGetWindowContentScale(window, &cx, &cy);
    g_scale_auto = ui_scale_auto(cy, framebuffer_height);
    apply_scale();
}

static void zoom(float delta) {
    g_camera.dist = fminf(fmaxf(g_camera.dist + delta, CAMERA_DIST_MIN), CAMERA_DIST_MAX);
}

static void turn(float yaw, float pitch) {
    g_camera.yaw += yaw;
    g_camera.pitch = fminf(fmaxf(g_camera.pitch + pitch, CAMERA_PITCH_MIN), CAMERA_PITCH_MAX);
}

static void scroll_log(int rows) {
    int s = g_ui.log_scroll + rows;
    if (s > LOG_SCROLL_MAX) {
        s = LOG_SCROLL_MAX;
    }
    g_ui.log_scroll = s < 0 ? 0 : s;
}

/*Sends a command as if typed, and shows it in the message panel.*/
static void run_command(GLFWwindow *window, const char *cmd) {
    char echo[LINE_EDIT_MAX + 4];
    snprintf(echo, sizeof(echo), "> %s", cmd);
    client_log_add(echo);
    g_ui.log_scroll = 0;
    if (strcmp(cmd, "quit") == 0) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }
    client_send_text_cmd(cmd);
}

static void do_action(GLFWwindow *window, ClientAction a, bool opened_by_char) {
    switch (a) {
    case ACT_MOVE_NORTH:
        client_send_move(0, -1);
        break;
    case ACT_MOVE_SOUTH:
        client_send_move(0, 1);
        break;
    case ACT_MOVE_WEST:
        client_send_move(-1, 0);
        break;
    case ACT_MOVE_EAST:
        client_send_move(1, 0);
        break;
    case ACT_ZOOM_IN:
        zoom(-1.5f);
        break;
    case ACT_ZOOM_OUT:
        zoom(1.5f);
        break;
    case ACT_CAMERA_LEFT:
        turn(-5.0f, 0.0f);
        break;
    case ACT_CAMERA_RIGHT:
        turn(5.0f, 0.0f);
        break;
    case ACT_CAMERA_UP:
        turn(0.0f, 3.0f);
        break;
    case ACT_CAMERA_DOWN:
        turn(0.0f, -3.0f);
        break;
    case ACT_COMMAND:
        g_ui.typing = true;
        g_ui.show_help = false;
        g_swallow_char = opened_by_char;
        break;
    case ACT_LOG_UP:
        scroll_log(LOG_SCROLL_STEP);
        break;
    case ACT_LOG_DOWN:
        scroll_log(-LOG_SCROLL_STEP);
        break;
    case ACT_SCALE_UP:
        g_scale_user = ui_scale_clamp(g_ui.scale + 0.25f);
        apply_scale();
        break;
    case ACT_SCALE_DOWN:
        g_scale_user = ui_scale_clamp(g_ui.scale - 0.25f);
        apply_scale();
        break;
    case ACT_TOGGLE_FPS:
        g_ui.show_fps = !g_ui.show_fps;
        break;
    case ACT_TOGGLE_PANELS:
        g_side_choice = g_ui.show_side ? -1 : 1;
        apply_scale();
        break;
    case ACT_HELP:
        g_ui.show_help = !g_ui.show_help;
        break;
    case ACT_QUIT:
        if (g_ui.show_help) {
            g_ui.show_help = false;
        } else {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        break;
    case ACT_NONE:
    case ACT_COUNT:
        break;
    }
}

/*A key while the command line is open: editing, history, send, cancel.*/
static void typing_key(GLFWwindow *window, int key, int mods) {
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    switch (key) {
    case GLFW_KEY_ESCAPE:
        line_edit_clear(&g_line);
        g_ui.typing = false;
        break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER: {
        char cmd[LINE_EDIT_MAX];
        g_ui.typing = false;
        if (line_edit_submit(&g_line, cmd, sizeof(cmd))) {
            run_command(window, cmd);
        }
        break;
    }
    case GLFW_KEY_BACKSPACE:
        line_edit_backspace(&g_line);
        break;
    case GLFW_KEY_DELETE:
        line_edit_delete(&g_line);
        break;
    case GLFW_KEY_LEFT:
        ctrl ? line_edit_word_left(&g_line) : line_edit_left(&g_line);
        break;
    case GLFW_KEY_RIGHT:
        ctrl ? line_edit_word_right(&g_line) : line_edit_right(&g_line);
        break;
    case GLFW_KEY_HOME:
        line_edit_home(&g_line);
        break;
    case GLFW_KEY_END:
        line_edit_end(&g_line);
        break;
    case GLFW_KEY_UP:
        line_edit_history_prev(&g_line);
        break;
    case GLFW_KEY_DOWN:
        line_edit_history_next(&g_line);
        break;
    case GLFW_KEY_PAGE_UP:
        scroll_log(LOG_SCROLL_STEP);
        break;
    case GLFW_KEY_PAGE_DOWN:
        scroll_log(-LOG_SCROLL_STEP);
        break;
    case GLFW_KEY_U:
        if (ctrl) {
            line_edit_kill_before(&g_line);
        }
        break;
    case GLFW_KEY_K:
        if (ctrl) {
            line_edit_kill_after(&g_line);
        }
        break;
    default:
        break;
    }
}

/*Does the key that opened the command line also produce a character?*/
static bool key_types_a_character(int key, int mods) {
    return key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT &&
           !(mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT));
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)scancode;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        return;
    }
    g_swallow_char = false;
    if (g_ui.typing) {
        typing_key(window, key, mods);
        return;
    }
    ClientAction a = client_config_action(g_cfg, key, mods);
    if (a != ACT_NONE) {
        do_action(window, a, key_types_a_character(key, mods));
        return;
    }
    const char *macro = client_config_macro(g_cfg, key, mods);
    if (macro && action == GLFW_PRESS) {
        run_command(window, macro);
    }
}

static void char_callback(GLFWwindow *window, unsigned int codepoint) {
    (void)window;
    if (!g_ui.typing) {
        return;
    }
    if (g_swallow_char) {
        g_swallow_char = false;
        return;
    }
    (void)line_edit_insert(&g_line, codepoint);
}

static void cursor_callback(GLFWwindow *window, double xpos, double ypos) {
    (void)window;
    if (g_mouse_held) {
        turn((float)(xpos - g_last_x) * 0.5f, (float)(ypos - g_last_y) * 0.5f);
    }
    g_last_x = xpos;
    g_last_y = ypos;
}

static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods) {
    (void)window;
    (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_mouse_held = action == GLFW_PRESS;
    }
}

static void scroll_callback(GLFWwindow *window, double xoff, double yoff) {
    (void)window;
    (void)xoff;
    zoom(-(float)yoff * 1.5f);
}

void client_input_install(GLFWwindow *window, const ClientConfig *cfg) {
    g_cfg = cfg;
    line_edit_init(&g_line);
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.line = &g_line;
    g_ui.show_fps = cfg->show_fps;
    g_ui.reduce_motion = cfg->reduce_motion;
    g_scale_user = cfg->ui_scale;
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    client_input_resized(window, fb_w, fb_h);
    glfwSetKeyCallback(window, key_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetCursorPosCallback(window, cursor_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetScrollCallback(window, scroll_callback);
}
