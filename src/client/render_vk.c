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
#include "client_tome.h"

static VkState vk_state;

#include "client_particles.h"
#include "render_gl.h"

typedef struct {
    float cur_x, cur_z;
    float tgt_x, tgt_z;
    bool  initialized;
} LerpPos;

static LerpPos g_entity_lerp[CLIENT_MAX_ENTITIES] = {0};

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

static void render_vk_hud(VkVertex *v, uint32_t *c, uint32_t max_v, float sw, float sh, float mvp[16], float px, float pz) {
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
        project_point(mvp, px, 0.5f, pz - 3.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "N", 1.0f, 1.0f, 0.2f, 0.2f);
        
        // South (Z = 3)
        project_point(mvp, px, 0.5f, pz + 3.0f, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "S", 1.0f, 0.8f, 0.8f, 0.8f);
        
        // East (X = 3)
        project_point(mvp, px + 3.0f, 0.5f, pz, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "E", 1.0f, 0.8f, 0.8f, 0.8f);
        
        // West (X = -3)
        project_point(mvp, px - 3.0f, 0.5f, pz, sw, sh, &sx, &sy, &vis);
        if (vis) draw_text_vk(v, c, max_v, sx - 4.0f, sy - 4.0f, "W", 1.0f, 0.8f, 0.8f, 0.8f);
    }

    /* Center reticle */
    float cx_c = sw * 0.5f;
    float cy_c = sh * 0.5f;
    draw_text_vk(v, c, max_v, cx_c - 20.0f, cy_c - 8.0f, "[",  scale, 0.2f, 1.0f, 0.2f);
    draw_text_vk(v, c, max_v, cx_c -  4.0f, cy_c - 8.0f, "+",  scale, 0.2f, 1.0f, 0.2f);
    draw_text_vk(v, c, max_v, cx_c + 12.0f, cy_c - 8.0f, "]",  scale, 0.2f, 1.0f, 0.2f);

    /* Player Names */
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        if (g_entities[i].active && g_entities[i].is_player && g_entities[i].id != g_my_entity_id && g_entities[i].floor_id == g_my_floor) {
            if (g_entities[i].username[0] != '\0') {
                float ex = (float)g_entities[i].x;
                float ez = (float)g_entities[i].y;
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

/* Box with an explicit orientation matrix (the animated floating tome).
 * Same 36-vertex box as push_box, but every corner and face normal is
 * rotated by the 3x3 part of `orient` (column-major 4x4 rotation, no
 * translation/scale), around the center (cx,cy,cz). */
static void push_box_oriented(VkVertex *v, uint32_t *c,
                              float cx, float cy, float cz,
                              float hx, float hy, float hz,
                              const float orient[16],
                              float r, float g, float b) {
    /* Transform the 8 corners: bit2 = +x, bit1 = +y, bit0 = +z */
    float X[8], Y[8], Z[8];
    for (int i = 0; i < 8; i++) {
        float lx = (i & 4) ?  hx : -hx;
        float ly = (i & 2) ?  hy : -hy;
        float lz = (i & 1) ?  hz : -hz;
        X[i] = cx + orient[0]*lx + orient[4]*ly + orient[8]*lz;
        Y[i] = cy + orient[1]*lx + orient[5]*ly + orient[9]*lz;
        Z[i] = cz + orient[2]*lx + orient[6]*ly + orient[10]*lz;
    }
    /* Transform the 6 face normals: 0=-x 1=+x 2=-y 3=+y 4=-z 5=+z */
    const float fn[6][3] = {
        {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}
    };
    float NX[6], NY[6], NZ[6];
    for (int f = 0; f < 6; f++) {
        NX[f] = orient[0]*fn[f][0] + orient[4]*fn[f][1] + orient[8]*fn[f][2];
        NY[f] = orient[1]*fn[f][0] + orient[5]*fn[f][1] + orient[9]*fn[f][2];
        NZ[f] = orient[2]*fn[f][0] + orient[6]*fn[f][1] + orient[10]*fn[f][2];
    }
    /* Faces: same corner order as push_box (top, bottom, front, back,
     * left, right), each as two triangles; index into the corner and
     * normal arrays. */
    const int faces[6][7] = {
        {2,6,3, 6,7,3, 3},
        {0,1,4, 4,1,5, 2},
        {1,5,3, 5,7,3, 5},
        {0,2,4, 4,2,6, 4},
        {0,1,2, 1,3,2, 0},
        {4,6,5, 5,6,7, 1},
    };
    for (int f = 0; f < 6; f++) {
        for (int t = 0; t < 6; t += 3) {
            push_vertex(v, c, X[faces[f][t+0]], Y[faces[f][t+0]], Z[faces[f][t+0]], r, g, b, NX[faces[f][6]], NY[faces[f][6]], NZ[faces[f][6]]);
            push_vertex(v, c, X[faces[f][t+1]], Y[faces[f][t+1]], Z[faces[f][t+1]], r, g, b, NX[faces[f][6]], NY[faces[f][6]], NZ[faces[f][6]]);
            push_vertex(v, c, X[faces[f][t+2]], Y[faces[f][t+2]], Z[faces[f][t+2]], r, g, b, NX[faces[f][6]], NY[faces[f][6]], NZ[faces[f][6]]);
        }
    }
}

/*The map mesh is built only for a WINDOW around the player, not the
 * whole 300x300 floor. The fragment shader DISCARDS every fragment
 * further than visionRadius (max 50 in the city), so geometry outside
 * that radius is never visible — yet the old code emitted all ~90 000
 * tiles and redid it on every map chunk, i.e. on EVERY step while
 * exploring: 3M+ vertices (~130 MB of coherent writes) per step, the
 * 100-130 ms CPU stall the player felt as stutter.
 *
 * Rebuild policy (simple because it is now cheap):
 *  - skip the rebuild when the built window still covers the player's
 *    required window and no tile data changed (g_map_dirty is set by
 *    the net thread ONLY when a chunk actually differs from
 *    g_local_map, so no-op chunks cost nothing);
 *  - otherwise rebuild the window around the current position with a
 *    BUILD_MARGIN, so a rebuild re-happens only after ~MARGIN tiles of
 *    travel (amortized), not on every step.
 * The persistent vertex buffer keeps the map block intact between
 * frames; only the entity/particle tail is rewritten every frame.*/
#define VK_MAP_VISION_MARGIN 8   /*fog fade-out slack beyond visionRadius */
#define VK_MAP_BUILD_MARGIN  16  /*rebuild amortization: tiles of travel  */
#define VK_MAP_MIN_RADIUS   16
#define VK_MAP_MAX_RADIUS   56   /*city visionRadius is 50 (50+8 -> 56)   */

static uint32_t s_map_vertex_count = 0;
static bool s_map_built = false;
static int s_map_floor = -1;
static int s_map_win_x0 = 0, s_map_win_y0 = 0;
static int s_map_win_x1 = -1, s_map_win_y1 = -1;

static void update_vertex_buffer(VkState *s, VkVertex *v, float dt, FrameSnapshot *snap) {
    uint32_t count = 0;
    int y, x;
    int px = (snap->my_x != -1) ? snap->my_x : 500;
    int py = (snap->my_y != -1) ? snap->my_y : 500;
    int vr = snap->vision_radius;
    bool full = false;

    if (snap->my_x >= 0 && snap->my_y >= 0) {
        int R = vr + VK_MAP_VISION_MARGIN;
        if (R < VK_MAP_MIN_RADIUS) R = VK_MAP_MIN_RADIUS;
        if (R > VK_MAP_MAX_RADIUS) R = VK_MAP_MAX_RADIUS;
        int need_x0 = snap->my_x - R, need_x1 = snap->my_x + R;
        int need_y0 = snap->my_y - R, need_y1 = snap->my_y + R;
        if (need_x0 < 0) need_x0 = 0;
        if (need_y0 < 0) need_y0 = 0;
        if (need_x1 >= MAP_WIDTH) need_x1 = MAP_WIDTH - 1;
        if (need_y1 >= MAP_HEIGHT) need_y1 = MAP_HEIGHT - 1;
        bool covers = s_map_built && s_map_floor == snap->my_floor &&
                      s_map_win_x0 <= need_x0 && s_map_win_x1 >= need_x1 &&
                      s_map_win_y0 <= need_y0 && s_map_win_y1 >= need_y1;
        if (g_map_dirty || !covers) {
            int bR = R + VK_MAP_BUILD_MARGIN;
            int x0 = snap->my_x - bR; if (x0 < 0) x0 = 0;
            int x1 = snap->my_x + bR; if (x1 >= MAP_WIDTH) x1 = MAP_WIDTH - 1;
            int y0 = snap->my_y - bR; if (y0 < 0) y0 = 0;
            int y1 = snap->my_y + bR; if (y1 >= MAP_HEIGHT) y1 = MAP_HEIGHT - 1;
            count = 0;
            for (y = y0; y <= y1 && !full; y++) {
                for (x = x0; x <= x1 && !full; x++) {
                    float fx = (float)x;
                    float fz = (float)y;
                    VoxelType tile = snap->map[y][x];
                    if (tile == 0) continue; // optimization

                    if (tile == VOXEL_WALL || tile == VOXEL_OBSIDIAN || tile == VOXEL_GOLD_VEIN) {
                        if (count + 36 > s->max_vertices) { full = true; break; }
                        if (tile == VOXEL_OBSIDIAN) push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.1f, 0.05f, 0.2f);
                        else if (tile == VOXEL_GOLD_VEIN) push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.8f, 0.7f, 0.1f);
                        else push_box(v, &count, fx, 0.5f, fz, 0.5f, 1.5f, 0.5f, 0.6f, 0.6f, 0.6f);
                    } else if (tile == VOXEL_FLOOR || tile == VOXEL_COBBLE || tile == VOXEL_WOOD || tile == VOXEL_ICE || tile == VOXEL_SAND || tile == VOXEL_ASH || tile == VOXEL_MUD || tile == VOXEL_MARBLE || tile == VOXEL_GRASS || tile == VOXEL_TRAP) {
                        if (count + 36 > s->max_vertices) { full = true; break; }
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
                        if (count + 36 > s->max_vertices) { full = true; break; }
                        float r = 1.0f, g = 1.0f, b = 1.0f;
                        if (tile == VOXEL_CRYSTAL_BLUE)   { r = 0.3f; g = 0.7f; b = 1.0f; }
                        if (tile == VOXEL_CRYSTAL_PURPLE)  { r = 0.8f; g = 0.2f; b = 1.0f; }
                        if (tile == VOXEL_CRYSTAL_RED)     { r = 1.0f; g = 0.1f; b = 0.1f; }
                        if (tile == VOXEL_CRYSTAL_GREEN)   { r = 0.1f; g = 1.0f; b = 0.2f; }
                        if (tile == VOXEL_CRYSTAL_YELLOW)  { r = 1.0f; g = 0.9f; b = 0.1f; }
                        if (tile == VOXEL_CRYSTAL_ORANGE)  { r = 1.0f; g = 0.5f; b = 0.0f; }
                        if (tile == VOXEL_CRYSTAL_CYAN)    { r = 0.0f; g = 0.9f; b = 1.0f; }
                        push_box(v, &count, fx, 0.8f, fz, 0.4f, 0.8f, 0.4f, r, g, b);
                    } else if (tile == VOXEL_WATER || tile == VOXEL_LAVA) {
                        if (count + 36 > s->max_vertices) { full = true; break; }
                        if (tile == VOXEL_WATER) push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.1f, 0.4f, 0.8f);
                        else push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 1.0f, 0.3f, 0.0f);
                    } else if (tile == VOXEL_DOOR) {
                        if (count + 72 <= s->max_vertices && !full) {
                            push_box(v, &count, fx, 0.4f, fz, 0.45f, 0.4f, 0.45f, 0.6f, 0.3f, 0.1f);
                            push_box(v, &count, fx, -0.05f, fz, 0.5f, 0.05f, 0.5f, 0.2f, 0.2f, 0.25f);
                        }
                    } else if (tile == VOXEL_STAIRS_DOWN || tile == VOXEL_STAIRS_UP) {
                        if (count + 36 > s->max_vertices) { full = true; break; }
                        push_box(v, &count, fx, 0.05f, fz, 0.5f, 0.1f, 0.5f, 0.9f, 0.9f, 0.0f);
                    } else if (tile == VOXEL_MUSHROOM_GLOW) {
                        if (count + 36 > s->max_vertices) { full = true; break; }
                        push_box(v, &count, fx, 0.2f, fz, 0.3f, 0.2f, 0.3f, 0.2f, 1.0f, 0.5f);
                    }
                }
            }
            s_map_vertex_count = count;
            s_map_built = true;
            s_map_floor = snap->my_floor;
            s_map_win_x0 = x0; s_map_win_y0 = y0;
            s_map_win_x1 = x1; s_map_win_y1 = y1;
            g_map_dirty = false;
        }
    } else {
        /*Not in the game yet: no map, and forget the previous floor.*/
        s_map_built = false;
        s_map_vertex_count = 0;
        s_map_floor = -1;
        s_map_win_x1 = -1;
        g_map_dirty = false;
    }
    count = s_map_vertex_count;

    // Rendering entities with lerp
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        if (snap->entities[i].active && snap->entities[i].id != snap->my_entity_id) {
            float tgt_ex = (float)snap->entities[i].x;
            float tgt_ez = (float)snap->entities[i].y;
            lerp_update(&g_entity_lerp[i], tgt_ex, tgt_ez, dt);
            float ex = g_entity_lerp[i].cur_x;
            float ez = g_entity_lerp[i].cur_z;
            
            if (fabs(ex - px) < (float)vr + 1.0f && fabs(ez - py) < (float)vr + 1.0f) {
                if (count + 36 <= s->max_vertices) {
                    float er = 0.4f, eg = 0.4f, eb = 1.0f;
                    if (snap->entities[i].is_merchant &&
                        snap->entities[i].shop_spec == SHOP_SPEC_BOOKS_MARTIAL) {
                        er = 0.75f; eg = 0.15f; eb = 0.2f;
                    }
                    else if (snap->entities[i].is_merchant) { er = 1.0f; eg = 0.8f; eb = 0.0f; }
                    else if (snap->entities[i].is_player) { er = 0.2f; eg = 0.8f; eb = 0.2f; }
                    else if (snap->entities[i].id < 10) { er = 1.0f; eg = 0.3f; eb = 0.3f; }
                                        if (snap->entities[i].is_player) {
                        push_pyramid(v, &count, ex, 0.4f, ez, 0.35f, 0.5f, 0.35f, er, eg, eb);
                    } else {
                        push_box(v, &count, ex, 0.4f, ez, 0.3f, 0.4f, 0.3f, er, eg, eb);
                        /* The Archive of a Thousand Battles: the tome
                         * flies a random trajectory INSIDE the shop
                         * (bounds measured from the map around the
                         * merchant) while performing a random rotation
                         * on itself — shared with the GL backend via
                         * client_tome.c */
                        if (snap->entities[i].is_merchant &&
                            snap->entities[i].shop_spec == SHOP_SPEC_BOOKS_MARTIAL &&
                            count + 36 <= s->max_vertices) {
                            float tp[3], to[16];
                            tome_anim_update(i, snap->entities[i].id, snap->entities[i].floor_id,
                                             snap->entities[i].x, snap->entities[i].y,
                                             snap->map[0], dt, tp, to);
                            push_box_oriented(v, &count,
                                             tp[0], tp[1], tp[2],
                                             0.45f, 0.1f, 0.35f, to,
                                             0.9f, 0.75f, 0.3f);
                        }
                    }
                }
            }
        } else if (!snap->entities[i].active) {
            g_entity_lerp[i].initialized = false;
            tome_anim_reset_slot(i);
        }
    }

    if (count + 36 <= s->max_vertices) {
        push_box(v, &count, (float)px, 0.6f, (float)py, 0.3f, 0.6f, 0.3f, 0.0f, 1.0f, 0.0f);
    }

    //Boss Trophies removed as per request.

    //Draw particles. NOTE: the SIMULATION (positions, life, alpha, and
    //freeing the slots) is NOT done here — it runs in draw_frame() via
    //particles_update(dt), BEFORE this vertex buffer is filled (A4).
    //Here we only map the pool into the VBO.
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g_particles[i].active) continue;
        Particle *p = &g_particles[i];

        p->a = p->life / p->max_life;
        
        // draw particle if space allows
        if (count + 36 <= s->max_vertices) {
            // we will draw them as floating boxes relative to (px, py)
            float fx = p->x;
            float fz = p->z;
            float fy = p->y;
            
            // To mimic additive blending/fade, we just multiply color by alpha for Vulkan
            float pr = p->r * p->a;
            float pg = p->g * p->a;
            float pb = p->b * p->a;
            
            push_box(v, &count, fx, -fy, fz, p->size, p->size, p->size, pr, pg, pb);
        }
    }

    /*No pthread_mutex_unlock here: update_vertex_buffer runs on the
     * RENDER thread over a private FrameSnapshot — frame_snapshot_acquire
     * already locked, copied and UNLOCKED the state mutex. The unlock
     * that used to be here was unmatched: releasing a mutex the calling
     * thread does not hold is undefined behavior (it can corrupt the
     * mutex and, with it, every other thread).*/
    s->vertex_count = count;
}

static void record_commands(VkState *s, float mvp[16], float vision_radius, float px, float pz, float time_val,
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
    float push_data[20];
    memcpy(push_data, mvp, sizeof(float) * 16);
    push_data[16] = vision_radius;
    push_data[17] = px;
    push_data[18] = pz;
    push_data[19] = time_val;
    vkCmdPushConstants(s->command_buffer, s->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 20, push_data);
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
    /*Three frames in flight: this frame uses slot `slot`. The only
     * blocking wait of the frame is below (the fence of the slot's LAST
     * use, 3 frames ago) — the old code instead waited for the fence of
     * the frame it had just submitted (vkWaitForFences(UINT64_MAX) at the
     * end of draw_frame), so the CPU and the GPU never overlapped.*/
    uint32_t slot = s->current_frame;
    uint32_t next_slot = (slot + 1) % MAX_FRAMES_IN_FLIGHT;

    uint32_t imageIndex = 0;
    VkResult acquire_res = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX,
                          s->sem_image[slot], VK_NULL_HANDLE, &imageIndex);
    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR || acquire_res == VK_SUBOPTIMAL_KHR) {
        /*Acquire happens BEFORE the fence wait/reset, so nothing of this
         * slot's resources has been touched: the fence is still in the
         * state left by the previous use (signalled) — safe to skip.*/
        s->current_frame = next_slot;
        return; /*deprecated swapchain: main loop recreates it*/
    }
    if (acquire_res != VK_SUCCESS && acquire_res != VK_TIMEOUT) {
        s->current_frame = next_slot;
        return; /*acquisition error: skip frame*/
    }

    /*The GPU must be done with the shared vertex buffer + command
     * buffer that this slot submitted 3 frames ago. From here on the
     * fence is UNSIGNALED, so every path that bails out must re-signal
     * it (see the empty submit below) or the next use of this slot
     * would wait forever.*/
    vkWaitForFences(s->device, 1, &s->fences[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(s->device, 1, &s->fences[slot]);
    vkResetCommandBuffer(s->command_buffer, 0);

    s->current_image = imageIndex;

    FrameSnapshot snap;
    frame_snapshot_acquire(&snap);
    float px = (snap.my_x != -1) ? (float)snap.my_x : 500.0f;
    float pz = (snap.my_y != -1) ? (float)snap.my_y : 500.0f;
    float vr = (float)snap.vision_radius;

    float rad_pitch = camera_pitch * (float)M_PI / 180.0f;
    float rad_yaw   = camera_yaw   * (float)M_PI / 180.0f;
    float center[3] = { px, 0.0f, pz };
    float eye[3] = {
        px + camera_dist * cosf(rad_pitch) * sinf(rad_yaw),
        0.0f + camera_dist * sinf(rad_pitch),
        pz + camera_dist * cosf(rad_pitch) * cosf(rad_yaw)
    };
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

    /*Use persistent mapping (3.4)*/
    VkVertex *v = (VkVertex *)s->mapped_vertex_data;
    if (!v) {
        VkSubmitInfo empty = {0};
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        vkQueueSubmit(s->graphics_queue, 1, &empty, s->fences[slot]);
        s->current_frame = next_slot;
        return;
    }

    /*A4: advance the particle simulation with this frame's dt. The GL
     *backend does the same in its render loop (render_gl.c), but the VK
     *path never did — the comment in update_vertex_buffer claimed the
     *update was "delegated to the main loop" and there it simply wasn't
     *called. Result: particles froze in place (alpha stuck at 1.0, no
     *motion, slots never freed), the 2000-slot pool saturated after a
     *few seconds of magic (one fireball alone spawns 200), and spawn_vfx
     *found no free slot again — no VFX at all until the client restarted.
     *Running it here, right before the VBO is filled from the pool, keeps
     *both backends on the exact same cadence (once per rendered frame).
     *Threading: identical to the GL backend — the net thread spawns via
     *spawn_vfx() (net_thread.c) while the render thread updates, a
     *tolerated loose race on a best-effort VFX pool, not made worse by
     *this change.*/
    particles_update(dt);

    update_vertex_buffer(s, v, dt, &snap);

    /*2D HUD: Write after 3D vertices*/
    uint32_t hud_start = s->vertex_count;
    uint32_t hud_count = hud_start;
    pthread_mutex_lock(&g_state_mutex);
    render_vk_hud(v, &hud_count, s->max_vertices, sw, sh, mvp, px, pz);
    pthread_mutex_unlock(&g_state_mutex);
    s->hud_vertex_count = hud_count - hud_start;

    // vkUnmapMemory(s->device, s->vertex_memory); // Replaced with persistent mapping

    record_commands(s, mvp, vr, px, pz, (float)current_time, hud_ortho, sw, sh);

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore waitSems[] = { s->sem_image[slot] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSems;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &s->command_buffer;
    VkSemaphore signalSems[] = { s->sem_render[slot] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSems;
    vkQueueSubmit(s->graphics_queue, 1, &submitInfo, s->fences[slot]);
    VkPresentInfoKHR presentInfo = {0};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSems;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &s->swapchain;
    presentInfo.pImageIndices = &imageIndex;
    vkQueuePresentKHR(s->present_queue, &presentInfo);
    /*No vkWaitForFences here: the next time this slot comes around
     * (3 frames later) the GPU will have signalled its fence long ago,
     * and the wait happens at the TOP of draw_frame. That is what lets
     * CPU and GPU pipeline.*/
    s->current_frame = next_slot;
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
