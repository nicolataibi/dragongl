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
#include "net.h"
#include "protocol.h"
#include "../../include/map.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include "font5x7.h"
#include "species.h"
#include "classes.h"
#include "client_minimap.h"

extern World g_world;
static VkState vk_state;

#include "client_particles.h"
#include "render_gl.h"

typedef struct {
    float cur_x, cur_z;
    float tgt_x, tgt_z;
    bool  initialized;
} LerpPos;

static LerpPos g_entity_lerp[MAX_NPCS] = {0};

static inline float lerp_f(float a, float b, float t) {
    return a + (b - a) * t;
}

static void lerp_update(LerpPos *lp, float tgt_x, float tgt_z, float dt) {
    if (!lp->initialized) {
        lp->cur_x = tgt_x;
        lp->cur_z = tgt_z;
        lp->tgt_x = tgt_x;
        lp->tgt_z = tgt_z;
        lp->initialized = true;
    } else {
        lp->tgt_x = tgt_x;
        lp->tgt_z = tgt_z;
        float LERP_SPEED = 5.0f;
        lp->cur_x = lerp_f(lp->cur_x, lp->tgt_x, dt * LERP_SPEED);
        lp->cur_z = lerp_f(lp->cur_z, lp->tgt_z, dt * LERP_SPEED);
    }
}

static float camera_yaw   = 0.0f;
static float camera_pitch = 45.0f;
static float camera_dist  = 25.0f;
static double last_x      = 0.0;
static double last_y      = 0.0;
static int    mouse_held  = 0;

static void cursor_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    if (mouse_held) {
        float dx = (float)(xpos - last_x);
        float dy = (float)(ypos - last_y);
        camera_yaw   += dx * 0.5f;
        camera_pitch  = fminf(fmaxf(camera_pitch + dy * 0.5f, 10.0f), 89.0f);
    }
    last_x = xpos;
    last_y = ypos;
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)window;
    (void)mods;
    
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        mouse_held = (action == GLFW_PRESS) ? 1 : 0;
    }
}

static void scroll_callback(GLFWwindow* window, double xoff, double yoff) {
    (void)window;
    (void)xoff;
    camera_dist -= (float)yoff * 1.5f;
    if (camera_dist < 2.0f)  camera_dist = 2.0f;
    if (camera_dist > 200.0f) camera_dist = 200.0f;
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_ESCAPE)    glfwSetWindowShouldClose(window, GLFW_TRUE);
        if (key == GLFW_KEY_PAGE_UP)   camera_dist -= 1.5f;
        if (key == GLFW_KEY_PAGE_DOWN) camera_dist += 1.5f;
        if (camera_dist < 2.0f)   camera_dist = 2.0f;
        if (camera_dist > 200.0f) camera_dist = 200.0f;
        if (key == GLFW_KEY_W) client_send_move(0, -1);
        if (key == GLFW_KEY_S) client_send_move(0,  1);
        if (key == GLFW_KEY_A) client_send_move(-1, 0);
        if (key == GLFW_KEY_D) client_send_move( 1, 0);
    }
}

static void mat4_mul(float c[16], const float a[16], const float b[16]) {
    float tmp[16];
    int col, row, k, i;
    for (i = 0; i < 16; i++) tmp[i] = 0.0f;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            for (k = 0; k < 4; k++) {
                tmp[col*4+row] += a[k*4+row] * b[col*4+k];
            }
        }
    }
    for (i = 0; i < 16; i++) c[i] = tmp[i];
}

static void mat4_perspective(float m[16], float fov_rad, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fov_rad * 0.5f);
    float A = zfar / (znear - zfar);
    float B = (znear * zfar) / (znear - zfar);
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0*4+0] = f / aspect;
    m[1*4+1] = -f;
    m[2*4+2] = A;  m[2*4+3] = -1.0f;
    m[3*4+2] = B;
}

/*2D orthographic projection: X into [0..w], Y into [0..h], Z into [-1..1]*/
static void mat4_ortho(float m[16], float w, float h) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0*4+0] =  2.0f / w;
    m[1*4+1] =  2.0f / h;
    m[2*4+2] = -1.0f;
    m[3*4+0] = -1.0f;
    m[3*4+1] = -1.0f;
    m[3*4+3] =  1.0f;
}

static void vec3_normalize(float v[3]) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) { v[0]/=len; v[1]/=len; v[2]/=len; }
}

static void vec3_cross(float r[3], const float a[3], const float b[3]) {
    r[0] = a[1]*b[2] - a[2]*b[1];
    r[1] = a[2]*b[0] - a[0]*b[2];
    r[2] = a[0]*b[1] - a[1]*b[0];
}

static float vec3_dot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void mat4_lookat(float m[16], const float eye[3], const float center[3], const float world_up[3]) {
    float f[3], s[3], u[3];
    int i;
    for (i = 0; i < 3; i++) f[i] = center[i] - eye[i];
    vec3_normalize(f);
    vec3_cross(s, f, world_up);
    vec3_normalize(s);
    vec3_cross(u, s, f);

    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0*4+0] = s[0]; m[1*4+0] = s[1]; m[2*4+0] = s[2];
    m[0*4+1] = u[0]; m[1*4+1] = u[1]; m[2*4+1] = u[2];
    m[0*4+2] = -f[0]; m[1*4+2] = -f[1]; m[2*4+2] = -f[2];
    m[3*4+0] = -vec3_dot(s, eye);
    m[3*4+1] = -vec3_dot(u, eye);
    m[3*4+2] =  vec3_dot(f, eye);
    m[3*4+3] =  1.0f;
}

static void push_vertex(VkVertex *v, uint32_t *c, float px, float py, float pz, float r, float g, float b, float nx, float ny, float nz) {
    v[*c].pos[0] = px; v[*c].pos[1] = py; v[*c].pos[2] = pz;
    v[*c].color[0] = r; v[*c].color[1] = g; v[*c].color[2] = b; v[*c].color[3] = 1.0f;
    v[*c].normal[0] = nx; v[*c].normal[1] = ny; v[*c].normal[2] = nz;
    (*c)++;
}

static void push_vertex_a(VkVertex *v, uint32_t *c, float px, float py, float r, float g, float b, float a) {
    v[*c].pos[0] = px; v[*c].pos[1] = py; v[*c].pos[2] = 0.0f;
    v[*c].color[0] = r; v[*c].color[1] = g; v[*c].color[2] = b; v[*c].color[3] = a;
    v[*c].normal[0] = 0.0f; v[*c].normal[1] = 0.0f; v[*c].normal[2] = 1.0f;
    (*c)++;
}

/*Draw a 2D quad for the HUD (pixel coordinates)*/
static void draw_quad_hud(VkVertex *v, uint32_t *c, uint32_t max_v,
                           float x, float y, float w, float h,
                           float r, float g, float b, float a) {
    if (*c + 6 > max_v) return;
    push_vertex_a(v, c, x,     y,     r, g, b, a);
    push_vertex_a(v, c, x + w, y,     r, g, b, a);
    push_vertex_a(v, c, x,     y + h, r, g, b, a);
    push_vertex_a(v, c, x + w, y,     r, g, b, a);
    push_vertex_a(v, c, x + w, y + h, r, g, b, a);
    push_vertex_a(v, c, x,     y + h, r, g, b, a);
}

/* Draws text using the font5x7 bitmap in the HUD vertex buffer */
static void draw_text_vk(VkVertex *v, uint32_t *c, uint32_t max_v,
                          float x, float y, const char *str, float scale,
                          float r, float g, float b) {
    float cursor_x = x;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch >= 32 && ch <= 127) {
            const uint8_t *glyph = font5x7[ch - 32];
            for (int col = 0; col < 5; col++) {
                uint8_t col_data = glyph[col];
                for (int row = 0; row < 7; row++) {
                    if (col_data & (1 << row)) {
                        float px = cursor_x + (float)col * scale;
                        float py = y + (float)row * scale;
                        draw_quad_hud(v, c, max_v, px, py, scale, scale, r, g, b, 1.0f);
                    }
                }
            }
        }
        cursor_x += 6.0f * scale;
        str++;
    }
}

/*Updates the minimap and draws it into the HUD vertex buffer*/
static void draw_minimap_vk(VkVertex *v, uint32_t *c, uint32_t max_v,
                             int px, int py, float ox, float oy, float cell_sz) {
    minimap_update(px, py, g_local_map, g_vision_radius);
    int buf_size = MINIMAP_BUF_SIZE;
    for (int by = 0; by < buf_size; by++) {
        for (int bx = 0; bx < buf_size; bx++) {
            MiniPixel mp = g_minimap_buf[by][bx];
            if (mp.a == 0) continue;
            float fr = (float)mp.r / 255.0f;
            float fg = (float)mp.g / 255.0f;
            float fb = (float)mp.b / 255.0f;
            float fa = (float)mp.a / 255.0f;
            float sx = ox + (float)bx * cell_sz;
            float sy = oy + (float)by * cell_sz;
            draw_quad_hud(v, c, max_v, sx, sy, cell_sz, cell_sz, fr, fg, fb, fa);
        }
    }
    /*Player point in the center (white)*/
    int center = MINIMAP_RADIUS;
    float cx_px = ox + (float)center * cell_sz;
    float cy_px = oy + (float)center * cell_sz;
    draw_quad_hud(v, c, max_v, cx_px - cell_sz, cy_px - cell_sz,
                  cell_sz * 3.0f, cell_sz * 3.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

/*=========================================================================
 * render_vk_hud — Generate 2D HUD vertices in the buffer
 * Mirror of render_gl_hud, adapted for 2D Vulkan vertices.
 * ========================================================================================*/

static void project_point(const float mvp[16], float x, float y, float z, float sw, float sh, float *sx, float *sy, bool *visible) {
    float clip_x = x * mvp[0] + y * mvp[4] + z * mvp[8]  + mvp[12];
    float clip_y = x * mvp[1] + y * mvp[5] + z * mvp[9]  + mvp[13];
    float clip_w = x * mvp[3] + y * mvp[7] + z * mvp[11] + mvp[15];
    
    if (clip_w <= 0.1f) {
        *visible = false;
        return;
    }
    
    float ndc_x = clip_x / clip_w;
    float ndc_y = clip_y / clip_w;
    
    *sx = (ndc_x + 1.0f) * 0.5f * sw;
    *sy = (ndc_y + 1.0f) * 0.5f * sh;
    
    *visible = (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f);
}

static void render_vk_hud(VkVertex *v, uint32_t *c, uint32_t max_v, float sw, float sh, float mvp[16]) {
    float sx = 20.0f;
    float sy = 20.0f;
    char buf[192];
    /* Techy Green */
    float th_r = 0.2f, th_g = 1.0f, th_b = 0.2f;
    float scale = 1.0f;

    /* Day/Night Clock */
    bool is_day = (g_game_h >= 6 && g_game_h < 20);
    float tr = is_day ? 1.0f : 0.3f;
    float tg = is_day ? 0.9f : 0.3f;
    float tb = is_day ? 0.2f : 1.0f;
    snprintf(buf, sizeof(buf), "TIME: %02d:00 [%s]", g_game_h, is_day ? "DAY" : "NIGHT");
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, tr, tg, tb);
    sy += 10.0f;

    /*Floor + Level + Race/Class*/
    const char *race_name  = (g_race_id  >= 0 && g_race_id  < RACE_COUNT)  ? RACES[g_race_id].name    : "";
    const char *class_name = (g_class_id >= 0 && g_class_id < CLASS_COUNT) ? CLASSES[g_class_id].name : "";
    snprintf(buf, sizeof(buf), "FLOOR: %d   LVL: %d  [%s %s]",
             g_my_floor, g_my_level, race_name, class_name);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b);
    sy += 12.0f;

    /* Position */
    snprintf(buf, sizeof(buf), "POS: %d,%d", g_my_x, g_my_y);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 0.5f, 0.9f, 0.5f);
    sy += 12.0f;

    /* Depth */
    snprintf(buf, sizeof(buf), "DEPTH: %d ft", g_my_floor * 50);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 0.6f, 0.6f, 0.6f);
    sy += 12.0f;

    /* HP */
    snprintf(buf, sizeof(buf), "HP: %d / %d", g_my_hp, g_my_max_hp);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 1.0f, 0.2f, 0.2f);
    sy += 12.0f;

    /* Vitality */
    const char *hunger_str = "SATIATED";
    float hr = 0.2f, hg = 1.0f, hb = 0.2f;
    if (g_hunger_level >= 1850) { hunger_str = "FAINTING"; hr = 1.0f; hg = 0.0f; hb = 0.0f; }
    else if (g_hunger_level >= 1600) { hunger_str = "WEAK";    hr = 1.0f; hg = 0.5f; hb = 0.0f; }
    else if (g_hunger_level >= 1200) { hunger_str = "HUNGRY";  hr = 1.0f; hg = 1.0f; hb = 0.0f; }
    snprintf(buf, sizeof(buf), "VITALITY: %s", hunger_str);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, hr, hg, hb);
    sy += 12.0f;

    /* Spell Slots */
    bool has_slots = false;
    for (int i = 1; i <= 9; i++) {
        if (g_my_spell_slots_max[i] > 0) { has_slots = true; break; }
    }
    if (has_slots) {
        sy += 10.0f;
        draw_text_vk(v, c, max_v, sx, sy, "SPELL SLOTS:", 1.0f, 0.5f, 0.8f, 1.0f);
        sy += 10.0f;
        for (int i = 1; i <= 9; i++) {
            if (g_my_spell_slots_max[i] > 0) {
                snprintf(buf, sizeof(buf), "L%d: %d/%d", i, g_my_spell_slots[i], g_my_spell_slots_max[i]);
                draw_text_vk(v, c, max_v, sx, sy, buf, 1.0f, 0.7f, 0.9f, 1.0f);
                sy += 10.0f;
            }
        }
    }

    /* Status Conditions */
    char statuses[256] = "";
    if (g_status_icons & (1 << 0))  strcat(statuses, "POISON ");
    if (g_status_icons & (1 << 1))  strcat(statuses, "BLIND ");
    if (g_status_icons & (1 << 2))  strcat(statuses, "PARALYZE ");
    if (g_status_icons & (1 << 3))  strcat(statuses, "STUN ");
    if (g_status_icons & (1 << 4))  strcat(statuses, "UNCONSCIOUS ");
    if (g_status_icons & (1 << 5))  strcat(statuses, "BURN ");
    if (g_status_icons & (1 << 6))  strcat(statuses, "BLEED ");
    if (g_status_icons & (1 << 7))  strcat(statuses, "PETRIFIED ");
    if (g_status_icons & (1 << 8))  strcat(statuses, "CURSE ");
    if (g_status_icons & (1 << 9))  strcat(statuses, "FROZEN ");
    if (g_status_icons & (1 << 10)) strcat(statuses, "EXHAUST ");
    if (g_status_icons & (1 << 11)) strcat(statuses, "STUDY ");
    if (statuses[0] != '\0') {
        snprintf(buf, sizeof(buf), "STATUS: %s", statuses);
        draw_text_vk(v, c, max_v, sx, sy, buf, 1.0f, 1.0f, 0.5f, 1.0f);
        sy += 10.0f;
    }

    draw_text_vk(v, c, max_v, sx, sy, "CONDITIONS: Normal", 1.0f, 0.7f, 0.7f, 0.7f);
    sy += 12.0f;

    /* XP & Gold */
    snprintf(buf, sizeof(buf), "XP: %d   GOLD: %lu", g_my_xp, (unsigned long)g_my_gold);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 1.0f, 0.8f, 0.1f);
    sy += 10.0f;

    /* Stats block */
    draw_text_vk(v, c, max_v, sx, sy, "--- STATS ---", scale, th_r, th_g, th_b); sy += 12.0f;
    snprintf(buf, sizeof(buf), "STR: %d", g_str);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 10.0f;
    snprintf(buf, sizeof(buf), "DEX: %d", g_dex);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 10.0f;
    snprintf(buf, sizeof(buf), "CON: %d", g_con);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 10.0f;
    snprintf(buf, sizeof(buf), "INT: %d", g_intel);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 10.0f;
    snprintf(buf, sizeof(buf), "WIS: %d", g_wis);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 10.0f;
    snprintf(buf, sizeof(buf), "CHA: %d", g_cha);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, th_r, th_g, th_b); sy += 16.0f;

    /* Combat section */
    draw_text_vk(v, c, max_v, sx, sy, "--- COMBAT ---", scale, th_r, th_g, th_b); sy += 12.0f;
    snprintf(buf, sizeof(buf), "Total AC  : %d", g_my_ac);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 0.5f, 0.8f, 1.0f); sy += 10.0f;
    snprintf(buf, sizeof(buf), "+To Hit   : %+d", g_to_hit);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 1.0f, 0.9f, 0.4f); sy += 10.0f;
    snprintf(buf, sizeof(buf), "+To Dmg   : %+d", g_to_dmg);
    draw_text_vk(v, c, max_v, sx, sy, buf, scale, 1.0f, 0.9f, 0.4f);

    /* =============================================================
     * RIGHT COLUMN: Equipment
     * ============================================================= */
    float rx = sw - 250.0f;
    float ry = 180.0f;
    draw_text_vk(v, c, max_v, rx, ry, "--- EQUIP ---", scale, th_r, th_g, th_b); ry += 10.0f;

    /* Helper: draws equipment slot */
    #define DRAW_SLOT_VK(label, name_str) do { \
        char _eq[64]; \
        if ((name_str)[0] != '\0') { \
            snprintf(_eq, sizeof(_eq), label " %.22s", name_str); \
            draw_text_vk(v, c, max_v, rx, ry, _eq, 1.0f, 0.9f, 0.85f, 0.75f); \
        } else { \
            draw_text_vk(v, c, max_v, rx, ry, label " ---", 1.0f, 0.35f, 0.35f, 0.35f); \
        } \
        ry += 10.0f; \
    } while(0)

    DRAW_SLOT_VK("HEA:", g_eq_head);
    DRAW_SLOT_VK("NEC:", g_eq_neck);
    DRAW_SLOT_VK("BDY:", g_eq_body);
    DRAW_SLOT_VK("BCK:", g_eq_back);
    DRAW_SLOT_VK("R.A:", g_eq_arm_r);
    DRAW_SLOT_VK("L.A:", g_eq_arm_l);
    DRAW_SLOT_VK("GLV:", g_eq_hands);
    DRAW_SLOT_VK("R.H:", g_eq_hand_r);
    DRAW_SLOT_VK("L.H:", g_eq_hand_l);
    DRAW_SLOT_VK("FET:", g_eq_feet);

    for (int ri = 0; ri < 10; ri++) {
        if (g_eq_ring[ri][0] != '\0') {
            snprintf(buf, sizeof(buf), "RNG: %.22s", g_eq_ring[ri]);
            draw_text_vk(v, c, max_v, rx, ry, buf, 1.0f, 0.9f, 0.85f, 0.75f);
            ry += 10.0f;
        }
    }
    for (int bi = 0; bi < 4; bi++) {
        if (g_eq_belt[bi][0] != '\0') {
            snprintf(buf, sizeof(buf), "BLT: %.22s", g_eq_belt[bi]);
            draw_text_vk(v, c, max_v, rx, ry, buf, 1.0f, 0.7f, 1.0f, 0.7f);
            ry += 10.0f;
        }
    }
    #undef DRAW_SLOT_VK

    /*=========================================================================================
* LOWER RIGHT SECTION: Boss Trophies
     * =========================================================================================*/
    {
        float bx = sw - 210.0f;
        float by = sh - 210.0f;
        draw_text_vk(v, c, max_v, bx, by - 22.0f, "--- BOSS TROPHIES ---", 1.0f, 1.0f, 0.7f, 0.1f);

        for (int bi = 0; bi < 10; bi++) {
            int col = bi % 5;
            int row_i = bi / 5;
            float bsx = bx + (float)col * 32.0f;
            float bsy = by + (float)row_i * 32.0f;
            bool defeated = (g_bosses_defeated & (1u << bi)) != 0;

            float cell_r = defeated ? 0.8f : 0.15f;
            float cell_g = defeated ? 0.5f : 0.15f;
            float cell_b = defeated ? 0.0f : 0.15f;
            draw_quad_hud(v, c, max_v, bsx, bsy, 28.0f, 28.0f, cell_r, cell_g, cell_b, 0.85f);

            if (defeated) {
                /* Stylized yellow diamond as 2 triangles */
                float dcx = bsx + 17.0f;
                float dcy = bsy + 10.0f;
                /* Top triangle */
                draw_quad_hud(v, c, max_v, dcx - 4.0f, dcy - 6.0f, 8.0f, 6.0f, 1.0f, 0.9f, 0.0f, 1.0f);
                /* Bottom triangle */
                draw_quad_hud(v, c, max_v, dcx - 4.0f, dcy, 8.0f, 6.0f, 1.0f, 0.9f, 0.0f, 1.0f);
            }

            char flabel[8];
            snprintf(flabel, sizeof(flabel), "F%d", (bi + 1) * 10);
            float fr2 = defeated ? 1.0f : 0.5f;
            float fg2 = defeated ? 0.9f : 0.5f;
            float fb2 = defeated ? 0.2f : 0.5f;
            draw_text_vk(v, c, max_v, bsx + 4.0f, bsy + 18.0f, flabel, 1.0f, fr2, fg2, fb2);
        }
    }

    /* =============================================================
     * MINIMAP (top right corner)
     * ============================================================= */
    if (g_my_x >= 0) {
        float mm_x = sw - MINIMAP_DISPLAY_SIZE - 10.0f;
        float mm_y = 10.0f;
        float cell_sz = (float)MINIMAP_DISPLAY_SIZE / (float)MINIMAP_BUF_SIZE;

        /* Semi-transparent background */
        draw_quad_hud(v, c, max_v, mm_x - 2.0f, mm_y - 2.0f,
                      (float)MINIMAP_DISPLAY_SIZE + 4.0f,
                      (float)MINIMAP_DISPLAY_SIZE + 4.0f,
                      0.0f, 0.0f, 0.0f, 0.7f);

        draw_minimap_vk(v, c, max_v, g_my_x, g_my_y, mm_x, mm_y, cell_sz);
    }



    /* =============================================================
     * AR COMPASS (3D -> 2D Projection)
     * ============================================================= */
    {
        float sx, sy;
        bool vis;
        // North (Z = -3)
        project_point(mvp, 0.0f, 0.5f, -3.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "N", 1.0f, 1.0f, 0.2f, 0.2f);
        
        // South (Z = 3)
        project_point(mvp, 0.0f, 0.5f, 3.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "S", 1.0f, 0.8f, 0.8f, 0.8f);
        
        // East (X = 3)
        project_point(mvp, 3.0f, 0.5f, 0.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "E", 1.0f, 0.8f, 0.8f, 0.8f);
        
        // West (X = -3)
        project_point(mvp, -3.0f, 0.5f, 0.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "W", 1.0f, 0.8f, 0.8f, 0.8f);
    }

    /* Center reticle */
    float cx_c = sw * 0.5f;
    float cy_c = sh * 0.5f;
    draw_text_vk(v, c, max_v, cx_c - 20.0f, cy_c - 8.0f, "[",  scale, 0.2f, 1.0f, 0.2f);
    draw_text_vk(v, c, max_v, cx_c -  4.0f, cy_c - 8.0f, "+",  scale, 0.2f, 1.0f, 0.2f);
    draw_text_vk(v, c, max_v, cx_c + 12.0f, cy_c - 8.0f, "]",  scale, 0.2f, 1.0f, 0.2f);

    /* Player Names */
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_entities[i].active && g_entities[i].is_player && g_entities[i].id != g_my_entity_id && g_entities[i].floor_id == g_my_floor) {
            if (g_entities[i].username[0] != '\0') {
                float ex = (float)(g_entities[i].x - g_my_x);
                float ez = (float)(g_entities[i].y - g_my_y);
                float psx, psy;
                bool pvis;
                project_point(mvp, ex, 1.2f, ez, sw, sh, &psx, &psy, &pvis);
                if (pvis) {
                    float len = strlen(g_entities[i].username) * 5.0f * 1.5f;
                    draw_text_vk(v, c, max_v, psx - (len / 2.0f), psy, g_entities[i].username, 1.5f, 0.4f, 1.0f, 0.4f);
                }
            }
        }
    }
}


static void push_box(VkVertex *v, uint32_t *c, float cx, float cy, float cz, float hx, float hy, float hz, float r, float g, float b) {
    float x1 = cx-hx, x2 = cx+hx;
    float y1 = cy-hy, y2 = cy+hy;
    float z1 = cz-hz, z2 = cz+hz;
    
    /* Top */
    push_vertex(v,c, x1,y2,z1, r,g,b, 0,1,0); push_vertex(v,c, x2,y2,z1, r,g,b, 0,1,0); push_vertex(v,c, x1,y2,z2, r,g,b, 0,1,0);
    push_vertex(v,c, x2,y2,z1, r,g,b, 0,1,0); push_vertex(v,c, x2,y2,z2, r,g,b, 0,1,0); push_vertex(v,c, x1,y2,z2, r,g,b, 0,1,0);
    /* Bottom */
    push_vertex(v,c, x1,y1,z1, r,g,b, 0,-1,0); push_vertex(v,c, x1,y1,z2, r,g,b, 0,-1,0); push_vertex(v,c, x2,y1,z1, r,g,b, 0,-1,0);
    push_vertex(v,c, x2,y1,z1, r,g,b, 0,-1,0); push_vertex(v,c, x1,y1,z2, r,g,b, 0,-1,0); push_vertex(v,c, x2,y1,z2, r,g,b, 0,-1,0);
    /* Front */
    push_vertex(v,c, x1,y1,z2, r,g,b, 0,0,1); push_vertex(v,c, x2,y1,z2, r,g,b, 0,0,1); push_vertex(v,c, x1,y2,z2, r,g,b, 0,0,1);
    push_vertex(v,c, x2,y1,z2, r,g,b, 0,0,1); push_vertex(v,c, x2,y2,z2, r,g,b, 0,0,1); push_vertex(v,c, x1,y2,z2, r,g,b, 0,0,1);
    /* Back */
    push_vertex(v,c, x1,y1,z1, r,g,b, 0,0,-1); push_vertex(v,c, x1,y2,z1, r,g,b, 0,0,-1); push_vertex(v,c, x2,y1,z1, r,g,b, 0,0,-1);
    push_vertex(v,c, x2,y1,z1, r,g,b, 0,0,-1); push_vertex(v,c, x1,y2,z1, r,g,b, 0,0,-1); push_vertex(v,c, x2,y2,z1, r,g,b, 0,0,-1);
    /* Left */
    push_vertex(v,c, x1,y1,z1, r,g,b, -1,0,0); push_vertex(v,c, x1,y1,z2, r,g,b, -1,0,0); push_vertex(v,c, x1,y2,z1, r,g,b, -1,0,0);
    push_vertex(v,c, x1,y1,z2, r,g,b, -1,0,0); push_vertex(v,c, x1,y2,z2, r,g,b, -1,0,0); push_vertex(v,c, x1,y2,z1, r,g,b, -1,0,0);
    /* Right */
    push_vertex(v,c, x2,y1,z1, r,g,b, 1,0,0); push_vertex(v,c, x2,y2,z1, r,g,b, 1,0,0); push_vertex(v,c, x2,y1,z2, r,g,b, 1,0,0);
    push_vertex(v,c, x2,y1,z2, r,g,b, 1,0,0); push_vertex(v,c, x2,y2,z1, r,g,b, 1,0,0); push_vertex(v,c, x2,y2,z2, r,g,b, 1,0,0);
}


static void push_pyramid(VkVertex *v, uint32_t *c, float cx, float cy, float cz, float hx, float hy, float hz, float r, float g, float b) {
    float x1 = cx - hx, x2 = cx + hx;
    float y1 = cy - hy, y2 = cy + hy;
    float z1 = cz - hz, z2 = cz + hz;

    // Front (Z2)
    push_vertex(v, c, cx, y2, cz, r, g, b, 0.0f, 0.5f, 1.0f);
    push_vertex(v, c, x1, y1, z2, r, g, b, 0.0f, 0.5f, 1.0f);
    push_vertex(v, c, x2, y1, z2, r, g, b, 0.0f, 0.5f, 1.0f);

    // Right (X2)
    push_vertex(v, c, cx, y2, cz, r, g, b, 1.0f, 0.5f, 0.0f);
    push_vertex(v, c, x2, y1, z2, r, g, b, 1.0f, 0.5f, 0.0f);
    push_vertex(v, c, x2, y1, z1, r, g, b, 1.0f, 0.5f, 0.0f);

    // Back (Z1)
    push_vertex(v, c, cx, y2, cz, r, g, b, 0.0f, 0.5f, -1.0f);
    push_vertex(v, c, x2, y1, z1, r, g, b, 0.0f, 0.5f, -1.0f);
    push_vertex(v, c, x1, y1, z1, r, g, b, 0.0f, 0.5f, -1.0f);

    // Left (X1)
    push_vertex(v, c, cx, y2, cz, r, g, b, -1.0f, 0.5f, 0.0f);
    push_vertex(v, c, x1, y1, z1, r, g, b, -1.0f, 0.5f, 0.0f);
    push_vertex(v, c, x1, y1, z2, r, g, b, -1.0f, 0.5f, 0.0f);

    // Base (Quad - 2 triangles)
    push_vertex(v, c, x1, y1, z1, r, g, b, 0.0f, -1.0f, 0.0f);
    push_vertex(v, c, x2, y1, z1, r, g, b, 0.0f, -1.0f, 0.0f);
    push_vertex(v, c, x1, y1, z2, r, g, b, 0.0f, -1.0f, 0.0f);
    push_vertex(v, c, x2, y1, z1, r, g, b, 0.0f, -1.0f, 0.0f);
    push_vertex(v, c, x2, y1, z2, r, g, b, 0.0f, -1.0f, 0.0f);
    push_vertex(v, c, x1, y1, z2, r, g, b, 0.0f, -1.0f, 0.0f);
}

static void update_vertex_buffer(VkState *s, VkVertex *v, float dt) {
    uint32_t count = 0;
    int y, x;
    int px, py;

    pthread_mutex_lock(&g_state_mutex);

    px = (g_my_x != -1) ? g_my_x : 500;
    py = (g_my_y != -1) ? g_my_y : 500;
    int vr = g_vision_radius;

    for (y = py - vr; y <= py + vr; y++) {
        for (x = px - vr; x <= px + vr; x++) {
            if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) continue;
            
            float fx = (float)(x - px);
            float fz = (float)(y - py);
            float d = sqrtf(fx*fx + fz*fz);
            if (d > (float)vr) continue;
            
            VoxelType tile = g_local_map[y][x];
            
            if (tile == VOXEL_WALL || tile == VOXEL_OBSIDIAN || tile == VOXEL_GOLD_VEIN) {
                if (count + 36 > s->max_vertices) break;
                if (tile == VOXEL_OBSIDIAN) push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.1f, 0.05f, 0.2f);
                else if (tile == VOXEL_GOLD_VEIN) push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.8f, 0.7f, 0.1f);
                else push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.6f, 0.6f, 0.6f);
            } else if (tile == VOXEL_FLOOR || tile == VOXEL_COBBLE || tile == VOXEL_WOOD || tile == VOXEL_ICE || tile == VOXEL_SAND || tile == VOXEL_ASH || tile == VOXEL_MUD || tile == VOXEL_MARBLE || tile == VOXEL_GRASS || tile == VOXEL_TRAP) {
                if (count + 36 > s->max_vertices) break;
                float r=0.2f, g=0.2f, b=0.2f;
                if (tile == VOXEL_WOOD) { r=0.4f; g=0.3f; b=0.2f; }
                if (tile == VOXEL_COBBLE) { r=0.3f; g=0.3f; b=0.3f; }
                if (tile == VOXEL_ICE) { r=0.6f; g=0.8f; b=1.0f; }
                if (tile == VOXEL_SAND) { r=0.8f; g=0.7f; b=0.4f; }
                if (tile == VOXEL_ASH) { r=0.25f; g=0.25f; b=0.25f; }
                if (tile == VOXEL_MUD) { r=0.3f; g=0.2f; b=0.1f; }
                if (tile == VOXEL_MARBLE) { r=0.9f; g=0.9f; b=0.9f; }
                if (tile == VOXEL_GRASS) { r=0.1f; g=0.5f; b=0.1f; }
                if (tile == VOXEL_TRAP) { r=0.8f; g=0.2f; b=0.1f; }
                push_box(v, &count, fx, 0.0f, fz, 0.5f, 0.1f, 0.5f, r, g, b);
            } else if (tile >= VOXEL_CRYSTAL_BLUE && tile <= VOXEL_CRYSTAL_WHITE) {
                if (count + 36 > s->max_vertices) break;
                float r = 1.0f, g = 1.0f, b = 1.0f;
                if (tile == VOXEL_CRYSTAL_BLUE)   { r = 0.3f; g = 0.7f; b = 1.0f; }
                if (tile == VOXEL_CRYSTAL_PURPLE)  { r = 0.8f; g = 0.2f; b = 1.0f; }
                if (tile == VOXEL_CRYSTAL_RED)     { r = 1.0f; g = 0.1f; b = 0.1f; }
                if (tile == VOXEL_CRYSTAL_GREEN)   { r = 0.1f; g = 1.0f; b = 0.2f; }
                if (tile == VOXEL_CRYSTAL_YELLOW)  { r = 1.0f; g = 0.9f; b = 0.1f; }
                if (tile == VOXEL_CRYSTAL_ORANGE)  { r = 1.0f; g = 0.5f; b = 0.0f; }
                if (tile == VOXEL_CRYSTAL_CYAN)    { r = 0.0f; g = 0.9f; b = 1.0f; }
                push_box(v, &count, fx, 0.8f, fz, 0.4f, 0.8f, 0.4f, r, g, b);
            } else if (tile == VOXEL_WALL) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.5f, fz, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f);
            } else if (tile == VOXEL_FLOOR) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.2f, 0.2f, 0.25f);
            } else if (tile == VOXEL_OBSIDIAN) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.5f, fz, 0.5f, 0.5f, 0.5f, 0.1f, 0.05f, 0.2f);
            } else if (tile == VOXEL_GOLD_VEIN) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.5f, fz, 0.5f, 0.5f, 0.5f, 0.8f, 0.7f, 0.1f);
            } else if (tile == VOXEL_WATER || tile == VOXEL_LAVA) {
                if (count + 36 > s->max_vertices) break;
                float liquid_y = (float)sin(glfwGetTime() * 2.0 + fx + fz) * 0.1f;
                if (tile == VOXEL_WATER) push_box(v, &count, fx, liquid_y - 0.05f, fz, 0.5f, 0.05f, 0.5f, 0.1f, 0.4f, 0.8f);
                else push_box(v, &count, fx, liquid_y - 0.05f, fz, 0.5f, 0.05f, 0.5f, 1.0f, 0.3f, 0.0f);
            } else if (tile == VOXEL_DOOR) {
                /*Two boxes = 72 vertices: the guard must cover both*/
                if (count + 72 <= s->max_vertices) {
                    push_box(v, &count, fx, 0.4f, fz, 0.45f, 0.4f, 0.45f, 0.6f, 0.3f, 0.1f);
                    push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.2f, 0.2f, 0.25f);
                }
            } else if (tile == VOXEL_GRASS) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.1f, 0.5f, 0.1f);
            } else if (tile == VOXEL_STAIRS_DOWN || tile == VOXEL_STAIRS_UP) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.05f, fz, 0.5f, 0.1f, 0.5f, 0.9f, 0.9f, 0.0f);
            } else if (tile == VOXEL_TRAP) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.8f, 0.2f, 0.1f);
            } else if (tile == VOXEL_MUSHROOM_GLOW) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.2f, fz, 0.3f, 0.2f, 0.3f, 0.2f, 1.0f, 0.5f);
            } else if (tile >= VOXEL_CRYSTAL_BLUE && tile <= VOXEL_CRYSTAL_WHITE) {
                if (count + 36 > s->max_vertices) break;
                push_box(v, &count, fx, 0.8f, fz, 0.4f, 0.8f, 0.4f, 0.5f, 0.8f, 1.0f); // Simplification for crystals
            }
        }
    }

    // Rendering entities with lerp
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_entities[i].active && g_entities[i].id != g_my_entity_id) {
            float tgt_ex = (float)(g_entities[i].x - px);
            float tgt_ez = (float)(g_entities[i].y - py);
            lerp_update(&g_entity_lerp[i], tgt_ex, tgt_ez, dt);
            float ex = g_entity_lerp[i].cur_x;
            float ez = g_entity_lerp[i].cur_z;
            
            if (fabs(ex) < (float)vr + 1.0f && fabs(ez) < (float)vr + 1.0f) {
                if (count + 36 <= s->max_vertices) {
                    float er = 0.4f, eg = 0.4f, eb = 1.0f;
                    if (g_entities[i].is_merchant) { er = 1.0f; eg = 0.8f; eb = 0.0f; }
                    else if (g_entities[i].is_player) { er = 0.2f; eg = 0.8f; eb = 0.2f; }
                    else if (g_entities[i].id < 10) { er = 1.0f; eg = 0.3f; eb = 0.3f; }
                                        if (g_entities[i].is_player) {
                        push_pyramid(v, &count, ex, 0.4f, ez, 0.35f, 0.5f, 0.35f, er, eg, eb);
                    } else {
                        push_box(v, &count, ex, 0.4f, ez, 0.3f, 0.4f, 0.3f, er, eg, eb);
                    }
                }
            }
        } else if (!g_entities[i].active) {
            g_entity_lerp[i].initialized = false;
        }
    }

    if (count + 36 <= s->max_vertices) {
        push_box(v, &count, 0.0f, 0.6f, 0.0f, 0.3f, 0.6f, 0.3f, 0.0f, 1.0f, 0.0f);
    }

    //Boss Trophies removed as per request.

    //Update and draw particles (update delegated to the main loop, here only vbo mapping)
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g_particles[i].active) continue;
        Particle *p = &g_particles[i];

        p->a = p->life / p->max_life;
        
        // draw particle if space allows
        if (count + 36 <= s->max_vertices) {
            // we will draw them as floating boxes relative to (px, py)
            float fx = p->x - px;
            float fz = p->z - py;
            float fy = p->y;
            
            // To mimic additive blending/fade, we just multiply color by alpha for Vulkan
            float pr = p->r * p->a;
            float pg = p->g * p->a;
            float pb = p->b * p->a;
            
            push_box(v, &count, fx, -fy, fz, p->size, p->size, p->size, pr, pg, pb);
        }
    }

    pthread_mutex_unlock(&g_state_mutex);
    s->vertex_count = count;
}

static void record_commands(VkState *s, float mvp[16], float vision_radius,
                             float hud_ortho[16], float sw, float sh) {
    (void)sw; (void)sh;
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(s->command_buffer, &beginInfo);
    VkClearValue clear_vals[2];
    clear_vals[0].color.float32[0] = 0.05f;
    clear_vals[0].color.float32[1] = 0.05f;
    clear_vals[0].color.float32[2] = 0.08f;
    clear_vals[0].color.float32[3] = 1.0f;
    clear_vals[1].depthStencil.depth = 1.0f;
    clear_vals[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo rpInfo = {0};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = s->render_pass;
    rpInfo.framebuffer = s->framebuffers[s->current_image];
    rpInfo.renderArea.extent = s->swapchain_extent;
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clear_vals;
    vkCmdBeginRenderPass(s->command_buffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    /* --- Step 1: 3D Scene --- */
    vkCmdBindPipeline(s->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    float push_data[17];
    memcpy(push_data, mvp, sizeof(float) * 16);
    push_data[16] = vision_radius;
    vkCmdPushConstants(s->command_buffer, s->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 17, push_data);
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(s->command_buffer, 0, 1, &s->vertex_buffer, offsets);
    if (s->vertex_count > 0) {
        vkCmdDraw(s->command_buffer, s->vertex_count, 1, 0, 0);
    }

    /*--- Step 2: 2D HUD overlay (pipeline without depth test, with blend) ---*/
    if (s->hud_vertex_count > 0 && s->pipeline_hud != VK_NULL_HANDLE) {
        vkCmdBindPipeline(s->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_hud);
        float hud_push[17];
        memcpy(hud_push, hud_ortho, sizeof(float) * 16);
        hud_push[16] = 99999.0f; /* disables fog in the fragment shader */
        vkCmdPushConstants(s->command_buffer, s->pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 17, hud_push);
        vkCmdDraw(s->command_buffer, s->hud_vertex_count, 1, s->vertex_count, 0);
    }

    vkCmdEndRenderPass(s->command_buffer);
    vkEndCommandBuffer(s->command_buffer);
}

static void draw_frame(VkState *s) {
    uint32_t imageIndex = 0;
    VkResult acquire_res = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX,
                          s->sem_image, VK_NULL_HANDLE, &imageIndex);
    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR || acquire_res == VK_SUBOPTIMAL_KHR) {
        return; /*deprecated swapchain: main loop recreates it*/
    }
    if (acquire_res != VK_SUCCESS && acquire_res != VK_TIMEOUT) {
        return; /*acquisition error: skip frame*/
    }
    s->current_image = imageIndex;

    float rad_pitch = camera_pitch * (float)M_PI / 180.0f;
    float rad_yaw   = camera_yaw   * (float)M_PI / 180.0f;
    float eye[3] = {
        camera_dist * cosf(rad_pitch) * sinf(rad_yaw),
        camera_dist * sinf(rad_pitch),
        camera_dist * cosf(rad_pitch) * cosf(rad_yaw)
    };
    float center[3] = { 0.0f, 0.0f, 0.0f };
    float up[3]     = { 0.0f, 1.0f, 0.0f };
    float view[16], proj[16], mvp[16];
    mat4_lookat(view, eye, center, up);
    float sw = (float)s->swapchain_extent.width;
    float sh = (float)s->swapchain_extent.height;
    float ratio = sw / sh;
    mat4_perspective(proj, 45.0f * (float)M_PI / 180.0f, ratio, 0.5f, 500.0f);
    mat4_mul(mvp, proj, view);

    /*Orthographic matrix for 2D HUD*/
    float hud_ortho[16];
    mat4_ortho(hud_ortho, sw, sh);

    static double last_time = 0.0;
    double current_time = glfwGetTime();
    if (last_time == 0.0) last_time = current_time;
    float dt = (float)(current_time - last_time);
    if (dt > 0.1f) dt = 0.1f;
    last_time = current_time;

    /*Single mapping for 3D scene + HUD*/
    VkVertex *v = NULL;
    if (vkMapMemory(s->device, s->vertex_memory, 0,
                    sizeof(VkVertex) * s->max_vertices, 0, (void **)&v) != VK_SUCCESS || !v) {
        return; /*mapping failed: skip frame*/
    }

    update_vertex_buffer(s, v, dt);

    /*2D HUD: Write after 3D vertices*/
    uint32_t hud_start = s->vertex_count;
    uint32_t hud_count = hud_start;
    pthread_mutex_lock(&g_state_mutex);
    render_vk_hud(v, &hud_count, s->max_vertices, sw, sh, mvp);
    pthread_mutex_unlock(&g_state_mutex);
    s->hud_vertex_count = hud_count - hud_start;

    vkUnmapMemory(s->device, s->vertex_memory);

    record_commands(s, mvp, (float)g_vision_radius, hud_ortho, sw, sh);

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore waitSems[] = { s->sem_image };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSems;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &s->command_buffer;
    VkSemaphore signalSems[] = { s->sem_render };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSems;
    vkResetFences(s->device, 1, &s->fence_flight);
    vkQueueSubmit(s->graphics_queue, 1, &submitInfo, s->fence_flight);
    VkPresentInfoKHR presentInfo = {0};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSems;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &s->swapchain;
    presentInfo.pImageIndices = &imageIndex;
    vkQueuePresentKHR(s->present_queue, &presentInfo);
    vkWaitForFences(s->device, 1, &s->fence_flight, VK_TRUE, UINT64_MAX);
}

void render_vk_start(void) {
    if (!glfwInit()) {
        printf("GLFW initialization error.\n");
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    vk_state.window = glfwCreateWindow(1920, 1080, "DragonGL - Vulkan 3D", NULL, NULL);
    if (!vk_state.window) {
        printf("Error creating GLFW window.\n");
        glfwTerminate();
        return;
    }
    glfwSetKeyCallback(vk_state.window, key_callback);
    glfwSetCursorPosCallback(vk_state.window, cursor_callback);
    glfwSetMouseButtonCallback(vk_state.window, mouse_button_callback);
    glfwSetScrollCallback(vk_state.window, scroll_callback);

    if (!vk_init(&vk_state)) {
        glfwDestroyWindow(vk_state.window);
        glfwTerminate();
        return;
    }

    while (!glfwWindowShouldClose(vk_state.window) && g_running) {
        glfwPollEvents();

        /*Resize gesture: Recreate the swapchain if the size has changed*/
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(vk_state.window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 &&
            ((uint32_t)fb_w != vk_state.swapchain_extent.width ||
             (uint32_t)fb_h != vk_state.swapchain_extent.height)) {
            if (!vk_recreate_swapchain(&vk_state, (uint32_t)fb_w, (uint32_t)fb_h)) {
                break; /*swapchain not recoverable: I exit the loop*/
            }
        }

        draw_frame(&vk_state);
    }

    vkDeviceWaitIdle(vk_state.device);
    vk_cleanup(&vk_state);
    glfwDestroyWindow(vk_state.window);
    glfwTerminate();
}
