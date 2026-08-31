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

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <GLFW/glfw3.h>
#include <GL/glu.h>
#include <pthread.h>
#include "client_state.h"
#include "font5x7.h"
#include "species.h"
#include "classes.h"

#include <stdlib.h>
#include "client_fct.h"
#include "client_minimap.h"
#include "client_particles.h"

/* ─────────────────────── Client-Side Interpolation ─────────────────────── *
 * Every visible entity maintains a float "rendered" position (cur)        *
 * which progressively approaches the integer grid position (tgt).           *
 * The approach speed is LERP_SPEED frame⁻¹.                               *
 * ─────────────────────────────────────────────────────────────────────────*/
#define LERP_SPEED 12.0f  /*units per second — higher = snappier*/

typedef struct {
    float cur_x, cur_z;   /*current rendered position (float)*/
    float tgt_x, tgt_z;   /* target position (grid integer → float)          */
    bool  initialized;
} LerpPos;

/*Interpolated player position*/
static LerpPos g_player_lerp = {0};

/* Interpolated positions of all entities */
static LerpPos g_entity_lerp[MAX_NPCS] = {0};

static inline float lerp_f(float a, float b, float t) {
    return a + (b - a) * t;
}

/*Update a LerpPos towards its target given the delta-time*/
static void lerp_update(LerpPos *lp, float tgt_x, float tgt_z, float dt) {
    if (!lp->initialized) {
        lp->cur_x = tgt_x;
        lp->cur_z = tgt_z;
        lp->initialized = true;
    }
    lp->tgt_x = tgt_x;
    lp->tgt_z = tgt_z;
    float t = fminf(1.0f, LERP_SPEED * dt);
    lp->cur_x = lerp_f(lp->cur_x, lp->tgt_x, t);
    lp->cur_z = lerp_f(lp->cur_z, lp->tgt_z, t);
}



static void draw_particles() {
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive blending for magic
    glDepthMask(GL_FALSE); // don't write to depth buffer
    float px = (float)g_my_x;
    float pz = (float)g_my_y;

    glBegin(GL_QUADS);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g_particles[i].active) continue;
        Particle *p = &g_particles[i];

        float rel_x = p->x - px;
        float rel_z = p->z - pz;

        /*Apply fog of war and vignetting based on distance from center (player)*/
        float d = sqrtf(rel_x * rel_x + rel_z * rel_z);
        float vr = (float)g_vision_radius;
        if (d > vr) {
            p->active = 0; /*Beyond visual range, remove*/
            continue;
        }
        
        float t_fog = (d - vr * 0.7f) / (vr * 0.3f);
        if (t_fog < 0.0f) t_fog = 0.0f;
        if (t_fog > 1.0f) t_fog = 1.0f;
        float fog = 1.0f - (t_fog * t_fog * (3.0f - 2.0f * t_fog));
        float vignette = 1.0f - (d / (vr * 1.2f)) * 0.15f;
        if (vignette < 0.5f) vignette = 0.5f;
        if (vignette > 1.0f) vignette = 1.0f;
        float total_fade = fog * vignette;

        // render billboard-ish or just simple quad (simplified)
        glColor4f(p->r, p->g, p->b, p->a * total_fade);
        float s = p->size;
        glVertex3f(rel_x - s, p->y - s, rel_z);
        glVertex3f(rel_x + s, p->y - s, rel_z);
        glVertex3f(rel_x + s, p->y + s, rel_z);
        glVertex3f(rel_x - s, p->y + s, rel_z);
        
        glVertex3f(rel_x, p->y - s, rel_z - s);
        glVertex3f(rel_x, p->y - s, rel_z + s);
        glVertex3f(rel_x, p->y + s, rel_z + s);
        glVertex3f(rel_x, p->y + s, rel_z - s);
    }
    glEnd();
    
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float camera_yaw = 0.0f;
static float camera_pitch = 45.0f;
static float camera_dist = 25.0f;
static double last_x, last_y;
static float bg_flash = 0.0f;

static void cursor_callback(GLFWwindow* window, double xpos, double ypos) {
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        float dx = (float)(xpos - last_x);
        float dy = (float)(ypos - last_y);
        camera_yaw += dx * 0.5f;
        camera_pitch = fminf(fmaxf(camera_pitch + dy * 0.5f, 10.0f), 89.0f);
    }
    last_x = xpos;
    last_y = ypos;
}

/* Input callbacks */
static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)window;
    (void)scancode;
    (void)mods;
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) g_running = false;
        // WSAD movement
        if (key == GLFW_KEY_W) client_send_move(0, -1);
        if (key == GLFW_KEY_S) client_send_move(0, 1);
        if (key == GLFW_KEY_A) client_send_move(-1, 0);
        if (key == GLFW_KEY_D) client_send_move(1, 0);
    }
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    if (action == GLFW_PRESS) {
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        float start_x = (float)width - 5.0f * 45.0f - 10.0f;
        float start_y = (float)height - 2.0f * 45.0f - 10.0f;

        for (int i = 0; i < 10; i++) {
            int row = i / 5;
            int col = i % 5;
            float sx = start_x + col * 45.0f;
            float sy = start_y + row * 45.0f;

            if (xpos >= sx && xpos <= sx + 40.0f && ypos >= sy && ypos <= sy + 40.0f) {
                char cmd[64];
                if (button == GLFW_MOUSE_BUTTON_LEFT) {
                    sprintf(cmd, "use %d", i + 1);
                } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                    sprintf(cmd, "w %d", i + 1);
                } else {
                    continue;
                }
                client_send_text_cmd(cmd);
                return;
            }
        }
    }
}


static void draw_pyramid(float x, float y, float z, float sx, float sy, float sz) {
    glPushMatrix();
    glTranslatef(x, y, z);
    glScalef(sx, sy, sz);

    glBegin(GL_TRIANGLES);
    // Front face
    glNormal3f(0.0f, 0.5f, 1.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    
    // Right face
    glNormal3f(1.0f, 0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    
    // Back face
    glNormal3f(0.0f, 0.5f, -1.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    
    // Left face
    glNormal3f(-1.0f, 0.5f, 0.0f);
    glVertex3f(0.0f, 0.5f, 0.0f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glEnd();
    
    // Base (Quad)
    glBegin(GL_QUADS);
    glNormal3f(0.0f, -1.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, -0.5f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glEnd();

    glPopMatrix();
}
static void draw_cube(float x, float y, float z, float sx, float sy, float sz) {
    glPushMatrix();
    glTranslatef(x, y, z);
    glScalef(sx, sy, sz);
    glBegin(GL_QUADS);
        glNormal3f(0, 1, 0); glVertex3f(0.5, 0.5, -0.5); glVertex3f(-0.5, 0.5, -0.5); glVertex3f(-0.5, 0.5, 0.5); glVertex3f(0.5, 0.5, 0.5);
        glNormal3f(0, -1, 0); glVertex3f(0.5, -0.5, 0.5); glVertex3f(-0.5, -0.5, 0.5); glVertex3f(-0.5, -0.5, -0.5); glVertex3f(0.5, -0.5, -0.5);
        glNormal3f(0, 0, 1); glVertex3f(0.5, 0.5, 0.5); glVertex3f(-0.5, 0.5, 0.5); glVertex3f(-0.5, -0.5, 0.5); glVertex3f(0.5, -0.5, 0.5);
        glNormal3f(0, 0, -1); glVertex3f(0.5, -0.5, -0.5); glVertex3f(-0.5, -0.5, -0.5); glVertex3f(-0.5, 0.5, -0.5); glVertex3f(0.5, 0.5, -0.5);
        glNormal3f(1, 0, 0); glVertex3f(0.5, 0.5, -0.5); glVertex3f(0.5, 0.5, 0.5); glVertex3f(0.5, -0.5, 0.5); glVertex3f(0.5, -0.5, -0.5);
        glNormal3f(-1, 0, 0); glVertex3f(-0.5, 0.5, 0.5); glVertex3f(-0.5, 0.5, -0.5); glVertex3f(-0.5, -0.5, -0.5); glVertex3f(-0.5, -0.5, 0.5);
    glEnd();
    glPopMatrix();
}

static void draw_tile(float x, float z) {
    glBegin(GL_QUADS);
        glNormal3f(0, 1, 0);
        glVertex3f(x+0.5f, 0.0f, z-0.5f);
        glVertex3f(x-0.5f, 0.0f, z-0.5f);
        glVertex3f(x-0.5f, 0.0f, z+0.5f);
        glVertex3f(x+0.5f, 0.0f, z+0.5f);
    glEnd();
}


static void draw_text(float x, float y, const char* str, float scale, float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    float cursor_x = x;
    while (*str) {
        unsigned char c = *str;
        if (c >= 32 && c <= 127) {
            const uint8_t *glyph = font5x7[c - 32];
            for (int col = 0; col < 5; col++) {
                uint8_t col_data = glyph[col];
                for (int row = 0; row < 7; row++) {
                    if (col_data & (1 << row)) {
                        float px = cursor_x + col * scale;
                        float py = y + row * scale;
                        glVertex2f(px, py);
                        glVertex2f(px + scale, py);
                        glVertex2f(px + scale, py + scale);
                        glVertex2f(px, py + scale);
                    }
                }
            }
        }
        cursor_x += 6 * scale;
        str++;
    }
    glEnd();
}

void render_gl_hud(int width, int height) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, width, height, 0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    // Aircraft HUD style: Green vector text, minimal UI.
    float cx = (float)width / 2.0f;
    float cy = (float)height / 2.0f;

    // Center crosshair bracket: [ + ]
    draw_text(cx - 20, cy - 8, "[", 2.0f, 0.2f, 1.0f, 0.2f);
    draw_text(cx - 4, cy - 8, "+", 2.0f, 0.2f, 1.0f, 0.2f);
    draw_text(cx + 12, cy - 8, "]", 2.0f, 0.2f, 1.0f, 0.2f);

    float sx = 20.0f;
    float sy = 20.0f;
    char buf[128];
    float th_r = 0.2f, th_g = 1.0f, th_b = 0.2f; // Techy Green

    //Day/Night clock text
    bool is_day = (g_game_h >= 6 && g_game_h < 20);
    sprintf(buf, "TIME: %02d:00 [%s]", g_game_h, is_day ? "DAY" : "NIGHT");
    draw_text(sx, sy, buf, 2.0f, is_day ? 1.0f : 0.3f, is_day ? 0.9f : 0.3f, is_day ? 0.2f : 1.0f);
    sy += 30;

    //Header: Floor + LEVEL (Race Class)
    const char *race_name  = (g_race_id  >= 0 && g_race_id  < RACE_COUNT)  ? RACES[g_race_id].name      : "";
    const char *class_name = (g_class_id >= 0 && g_class_id < CLASS_COUNT) ? CLASSES[g_class_id].name   : "";
    sprintf(buf, "FLOOR: %d   LVL: %d  [%s %s]", g_my_floor, g_my_level, race_name, class_name);
    draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b);
    sy += 22;

    // Position
    sprintf(buf, "POS: %d,%d", g_my_x, g_my_y);
    draw_text(sx, sy, buf, 2.0f, 0.5f, 0.9f, 0.5f);
    sy += 22;

    // Dungeon Depth
    sprintf(buf, "DEPTH: %d ft", g_my_floor * 50);
    draw_text(sx, sy, buf, 2.0f, 0.6f, 0.6f, 0.6f);
    sy += 22;

    // HP
    sprintf(buf, "HP: %d / %d", g_my_hp, g_my_max_hp);
    draw_text(sx, sy, buf, 2.0f, 1.0f, 0.2f, 0.2f);
    sy += 20;

    // Vitality (Hunger)
    const char *hunger_str = "SATIATED";
    float hr=0.2f, hg=1.0f, hb=0.2f;
    if (g_hunger_level >= 1850) { hunger_str = "FAINTING"; hr=1.0f; hg=0.0f; hb=0.0f; }
    else if (g_hunger_level >= 1600) { hunger_str = "WEAK"; hr=1.0f; hg=0.5f; hb=0.0f; }
    else if (g_hunger_level >= 1200) { hunger_str = "HUNGRY"; hr=1.0f; hg=1.0f; hb=0.0f; }
    sprintf(buf, "VITALITY: %s", hunger_str);
    draw_text(sx, sy, buf, 2.0f, hr, hg, hb);
    sy += 20;

    // Spell Slots
    bool has_slots = false;
    for (int i = 1; i <= 9; i++) {
        if (g_my_spell_slots_max[i] > 0) has_slots = true;
    }
    if (has_slots) {
        sy += 10;
        draw_text(sx, sy, "SPELL SLOTS:", 1.5f, 0.5f, 0.8f, 1.0f);
        sy += 16;
        for (int i = 1; i <= 9; i++) {
            if (g_my_spell_slots_max[i] > 0) {
                sprintf(buf, "L%d: %d/%d", i, g_my_spell_slots[i], g_my_spell_slots_max[i]);
                draw_text(sx, sy, buf, 1.5f, 0.7f, 0.9f, 1.0f);
                sy += 16;
            }
        }
    }

    // Status Conditions
    char statuses[256] = "";
    if (g_status_icons & (1 << 0)) strcat(statuses, "POISON ");
    if (g_status_icons & (1 << 1)) strcat(statuses, "BLIND ");
    if (g_status_icons & (1 << 2)) strcat(statuses, "PARALYZE ");
    if (g_status_icons & (1 << 3)) strcat(statuses, "STUN ");
    if (g_status_icons & (1 << 4)) strcat(statuses, "UNCONSCIOUS ");
    if (g_status_icons & (1 << 5)) strcat(statuses, "BURN ");
    if (g_status_icons & (1 << 6)) strcat(statuses, "BLEED ");
    if (g_status_icons & (1 << 7)) strcat(statuses, "PETRIFIED ");
    if (g_status_icons & (1 << 8)) strcat(statuses, "CURSE ");
    if (g_status_icons & (1 << 9)) strcat(statuses, "FROZEN ");
    if (g_status_icons & (1 << 10)) strcat(statuses, "EXHAUST ");
    if (g_status_icons & (1 << 11)) strcat(statuses, "STUDY ");
    
    if (statuses[0] != '\0') {
        sprintf(buf, "STATUS: %s", statuses);
        draw_text(sx, sy, buf, 1.5f, 1.0f, 0.5f, 1.0f);
        sy += 18;
    }

    // Additional Conditions
    draw_text(sx, sy, "CONDITIONS: Normal", 1.5f, 0.7f, 0.7f, 0.7f);
    sy += 20;

    // XP & GOLD
    sprintf(buf, "XP: %d   GOLD: %lu", g_my_xp, (unsigned long)g_my_gold);
    draw_text(sx, sy, buf, 2.0f, 1.0f, 0.8f, 0.1f);
    sy += 30;

    // Stats block
    draw_text(sx, sy, "--- STATS ---", 2.0f, th_r, th_g, th_b); sy += 20;
    sprintf(buf, "STR: %d", g_str);   draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 18;
    sprintf(buf, "DEX: %d", g_dex);   draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 18;
    sprintf(buf, "CON: %d", g_con);   draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 18;
    sprintf(buf, "INT: %d", g_intel); draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 18;
    sprintf(buf, "WIS: %d", g_wis);   draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 18;
    sprintf(buf, "CHA: %d", g_cha);   draw_text(sx, sy, buf, 2.0f, th_r, th_g, th_b); sy += 28;

    // Combat section
    draw_text(sx, sy, "--- COMBAT ---", 2.0f, th_r, th_g, th_b); sy += 20;
    sprintf(buf, "Total AC  : %d", g_my_ac);
    draw_text(sx, sy, buf, 2.0f, 0.5f, 0.8f, 1.0f); sy += 18;
    sprintf(buf, "+To Hit   : %+d", g_to_hit);
    draw_text(sx, sy, buf, 2.0f, 1.0f, 0.9f, 0.4f); sy += 18;
    sprintf(buf, "+To Dmg   : %+d", g_to_dmg);
    draw_text(sx, sy, buf, 2.0f, 1.0f, 0.9f, 0.4f); sy += 28;

    // Equipment section (Top Right)
    float rx = (float)width - 250.0f;
    float ry = 20.0f;
    draw_text(rx, ry, "--- EQUIP ---", 2.0f, th_r, th_g, th_b); ry += 20;

    /* helper macro: draw a slot line only when the name is non-empty */
    #define DRAW_SLOT(label, name_str) \
        do { \
            char _eq_line[64]; \
            if ((name_str)[0] != '\0') { \
                snprintf(_eq_line, sizeof(_eq_line), label " %.22s", name_str); \
                draw_text(rx, ry, _eq_line, 1.5f, 0.9f, 0.85f, 0.75f); \
            } else { \
                snprintf(_eq_line, sizeof(_eq_line), label " ---"); \
                draw_text(rx, ry, _eq_line, 1.5f, 0.35f, 0.35f, 0.35f); \
            } \
            ry += 14; \
        } while(0)

    DRAW_SLOT("HEA:", g_eq_head);
    DRAW_SLOT("NEC:", g_eq_neck);
    DRAW_SLOT("BDY:", g_eq_body);
    DRAW_SLOT("BCK:", g_eq_back); 
    DRAW_SLOT("R.A:", g_eq_arm_r); /* right arm*/
    DRAW_SLOT("L.A:", g_eq_arm_l); /* left arm */
    DRAW_SLOT("GLV:", g_eq_hands);
    DRAW_SLOT("R.H:", g_eq_hand_r); /* right hand */
    DRAW_SLOT("L.H:", g_eq_hand_l); /* left hand */
    DRAW_SLOT("FET:", g_eq_feet);

    /* Rings: show only equipped ones */
    bool any_ring = false;
    for (int ri = 0; ri < 10; ri++) {
        if (g_eq_ring[ri][0] != '\0') {
            any_ring = true;
            break;
        }
    }
    if (any_ring) {
        for (int ri = 0; ri < 10; ri++) {
            if (g_eq_ring[ri][0] != '\0') {
                char _ring_line[64];
                snprintf(_ring_line, sizeof(_ring_line), "RNG: %.22s", g_eq_ring[ri]);
                draw_text(rx, ry, _ring_line, 1.5f, 0.9f, 0.85f, 0.75f);
                ry += 14;
            }
        }
    }
    
    /* Belt slots */
    bool any_belt = false;
    for (int bi = 0; bi < 4; bi++) {
        if (g_eq_belt[bi][0] != '\0') {
            any_belt = true;
            break;
        }
    }
    if (any_belt) {
        for (int bi = 0; bi < 4; bi++) {
            if (g_eq_belt[bi][0] != '\0') {
                char _belt_line[64];
                snprintf(_belt_line, sizeof(_belt_line), "BLT: %.22s", g_eq_belt[bi]);
                draw_text(rx, ry, _belt_line, 1.5f, 0.7f, 1.0f, 0.7f);
                ry += 14;
            }
        }
    }
    #undef DRAW_SLOT

    // Inventory Grid (Phase 5)
    float grid_start_x = (float)width - 5.0f * 45.0f - 10.0f;
    float grid_start_y = (float)height - 2.0f * 45.0f - 10.0f;
    draw_text(grid_start_x, grid_start_y - 20.0f, "--- INVENTORY ---", 1.5f, 0.6f, 0.6f, 0.6f);
    draw_text(grid_start_x, grid_start_y - 10.0f, "(LMB: Use | RMB: Equip)", 1.0f, 0.5f, 0.5f, 0.5f);

    for (int i = 0; i < 10; i++) {
        int row = i / 5;
        int col = i % 5;
        float sx = grid_start_x + col * 45.0f;
        float sy = grid_start_y + row * 45.0f;
        
        // Cell background
        glColor4f(0.1f, 0.1f, 0.15f, 0.8f);
        glBegin(GL_QUADS);
        glVertex2f(sx, sy);
        glVertex2f(sx + 40.0f, sy);
        glVertex2f(sx + 40.0f, sy + 40.0f);
        glVertex2f(sx, sy + 40.0f);
        glEnd();
        
        // Border
        glColor4f(0.4f, 0.4f, 0.5f, 1.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(sx, sy);
        glVertex2f(sx + 40.0f, sy);
        glVertex2f(sx + 40.0f, sy + 40.0f);
        glVertex2f(sx, sy + 40.0f);
        glEnd();
        
        //Slot ID text
        char slot_text[4];
        sprintf(slot_text, "%d", i + 1);
        draw_text(sx + 14.0f, sy + 25.0f, slot_text, 1.5f, 0.8f, 0.8f, 0.8f);
    }



    //─── BOSS TROPHIES PANEL (right side, above inventory) ───────────────
    {
        float bx = (float)width - 210.0f;
        float by = (float)height - 210.0f;
        draw_text(bx, by - 22.0f, "--- BOSS TROPHIES ---", 1.5f, 1.0f, 0.7f, 0.1f);

        //10 boss slots (floors 10, 20 ... 100)
        for (int bi = 0; bi < 10; bi++) {
            int col = bi % 5;
            int row = bi / 5;
            float bsx = bx + col * 40.0f;
            float bsy = by + row * 40.0f;
            bool defeated = (g_bosses_defeated & (1u << bi)) != 0;

            float cell_r = defeated ? 0.8f : 0.15f;
            float cell_g = defeated ? 0.5f : 0.15f;
            float cell_b = defeated ? 0.0f : 0.15f;
            float border_r = defeated ? 1.0f : 0.3f;
            float border_g = defeated ? 0.75f : 0.3f;
            float border_b = defeated ? 0.1f : 0.3f;

            // Background quad
            glColor4f(cell_r, cell_g, cell_b, 0.85f);
            glBegin(GL_QUADS);
            glVertex2f(bsx,         bsy);
            glVertex2f(bsx + 34.0f, bsy);
            glVertex2f(bsx + 34.0f, bsy + 32.0f);
            glVertex2f(bsx,         bsy + 32.0f);
            glEnd();

            // Diamond indicator (inner glow for defeated)
            if (defeated) {
                float cx2 = bsx + 17.0f;
                float cy2 = bsy + 10.0f;
                glColor4f(1.0f, 0.9f, 0.0f, 1.0f);
                glBegin(GL_TRIANGLES);
                glVertex2f(cx2,        cy2 - 6.0f);
                glVertex2f(cx2 + 6.0f, cy2);
                glVertex2f(cx2 - 6.0f, cy2);
                glEnd();
                glBegin(GL_TRIANGLES);
                glVertex2f(cx2 + 6.0f, cy2);
                glVertex2f(cx2 - 6.0f, cy2);
                glVertex2f(cx2,        cy2 + 6.0f);
                glEnd();
            }

            // Border
            glColor4f(border_r, border_g, border_b, 1.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(bsx,         bsy);
            glVertex2f(bsx + 34.0f, bsy);
            glVertex2f(bsx + 34.0f, bsy + 32.0f);
            glVertex2f(bsx,         bsy + 32.0f);
            glEnd();

            // Floor label (e.g. "F10")
            char flabel[8];
            sprintf(flabel, "F%d", (bi + 1) * 10);
            float fr = defeated ? 1.0f : 0.5f;
            float fg = defeated ? 0.9f : 0.5f;
            float fb = defeated ? 0.2f : 0.5f;
            draw_text(bsx + 4.0f, bsy + 18.0f, flabel, 1.5f, fr, fg, fb);
        }
    }

    // Restore state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void render_gl_hud_window(int width, int height) {
    glViewport(0, 0, width, height);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    render_gl_hud(width, height);
}

/*─────────────────────── Floating Combat Text (GL) ─────────────────────── *
 * Draws floating numbers above entities using 2D projection.               *
 * The texts rise upwards and gradually fade away.                  *
* Cost: a single pass over the g_fct[] array (64 elements).           *
 * ───────────────────────────────────── ─────────────────────────────────────*/
static void draw_floating_combat_text(int width, int height) {
    /*Enter 2D orthographic projection*/
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, width, height, 0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    float cx = (float)width / 2.0f;
    float cy = (float)height / 2.0f;

    for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
        if (!g_fct[i].active) {
            continue;
        }
        const FloatingText *f = &g_fct[i];

        /*Convert world position → screen (centered approximation)*/
        float screen_x = cx + f->world_x * 40.0f;
        float screen_y = cy + f->world_z * 40.0f - f->offset_y * 30.0f;

        /*Select color based on type*/
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        switch (f->type) {
            case FCT_DAMAGE:
                r = 1.0f;
                g = 0.2f;
                b = 0.2f;
                break;
            case FCT_DAMAGE_OUT:
                r = 1.0f;
                g = 0.6f;
                b = 0.1f;
                break;
            case FCT_HEAL:
                r = 0.2f;
                g = 1.0f;
                b = 0.3f;
                break;
            case FCT_CRITICAL:
                r = 1.0f;
                g = 1.0f;
                b = 0.0f;
                break;
            case FCT_XP:
                r = 0.3f;
                g = 0.8f;
                b = 1.0f;
                break;
            case FCT_GOLD:
                r = 1.0f;
                g = 0.85f;
                b = 0.0f;
                break;
            case FCT_MISS:
                r = 0.6f;
                g = 0.6f;
                b = 0.6f;
                break;
            case FCT_LEVELUP:
                r = 1.0f;
                g = 1.0f;
                b = 1.0f;
                break;
        }

        /*Apply alpha (fade-out) by multiplying the color*/
        r *= f->alpha;
        g *= f->alpha;
        b *= f->alpha;

        draw_text(screen_x, screen_y, f->text, f->scale, r, g, b);
    }

    /*Restore state*/
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

/* ─────────────────────── Minimap Radar (GL) ─────────────────────────────── *
 * Draws the minimap in the top-right corner as a 2D overlay.                *
 * Each minimap pixel corresponds to a dungeon tile.                         *
 * The player is shown as a blinking green dot at the center.               *
 * ────────────────────────────────────────────────────────────────────────── */
static void draw_minimap_gl(int width, int height) {
    /*Position and size of the box on the screen*/
    float margin = 10.0f;
    float size = (float)MINIMAP_DISPLAY_SIZE;
    float x0 = (float)width - size - margin;
    //Compute y0 so the minimap sits above the Boss Trophies panel
    float boss_panel_label_y = (float)height - 210.0f - 22.0f;
    float y0 = boss_panel_label_y - size - 20.0f;

    /*Enter 2D orthographic projection*/
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, width, height, 0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /*Semi-transparent minimap background*/
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.05f, 0.7f);
    glBegin(GL_QUADS);
        glVertex2f(x0 - 2.0f, y0 - 2.0f);
        glVertex2f(x0 + size + 2.0f, y0 - 2.0f);
        glVertex2f(x0 + size + 2.0f, y0 + size + 2.0f);
        glVertex2f(x0 - 2.0f, y0 + size + 2.0f);
    glEnd();

    /*Pane border (dark green, HUD style)*/
    glColor4f(0.1f, 0.6f, 0.1f, 0.8f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
        glVertex2f(x0 - 2.0f, y0 - 2.0f);
        glVertex2f(x0 + size + 2.0f, y0 - 2.0f);
        glVertex2f(x0 + size + 2.0f, y0 + size + 2.0f);
        glVertex2f(x0 - 2.0f, y0 + size + 2.0f);
    glEnd();
    glLineWidth(1.0f);

    /*Draw the minimap pixels*/
    float pixel_size = size / (float)MINIMAP_BUF_SIZE;
    glBegin(GL_QUADS);
    for (int my = 0; my < MINIMAP_BUF_SIZE; my++) {
        for (int mx = 0; mx < MINIMAP_BUF_SIZE; mx++) {
            MiniPixel *p = &g_minimap_buf[my][mx];
            if (p->a == 0) {
                continue;
            }
            float cr = (float)p->r / 255.0f;
            float cg = (float)p->g / 255.0f;
            float cb = (float)p->b / 255.0f;
            float ca = (float)p->a / 255.0f;
            glColor4f(cr, cg, cb, ca);

            float px = x0 + (float)mx * pixel_size;
            float py = y0 + (float)my * pixel_size;
            glVertex2f(px, py);
            glVertex2f(px + pixel_size, py);
            glVertex2f(px + pixel_size, py + pixel_size);
            glVertex2f(px, py + pixel_size);
        }
    }
    glEnd();

    /*Player Point (center, flashing green)*/
    float pulse = 0.7f + 0.3f * sinf((float)glfwGetTime() * 6.0f);
    float center_x = x0 + size / 2.0f;
    float center_y = y0 + size / 2.0f;
    float dot = pixel_size * 1.5f;
    glColor4f(0.2f, pulse, 0.2f, 1.0f);
    glBegin(GL_QUADS);
        glVertex2f(center_x - dot, center_y - dot);
        glVertex2f(center_x + dot, center_y - dot);
        glVertex2f(center_x + dot, center_y + dot);
        glVertex2f(center_x - dot, center_y + dot);
    glEnd();

    /* Visible entities on the minimap (red/gold/purple dots) */
    for (int i = 0; i < MAX_NPCS; i++) {
        if (!g_entities[i].active || g_entities[i].id == g_my_entity_id) {
            continue;
        }
        int dx = g_entities[i].x - g_my_x;
        int dy = g_entities[i].y - g_my_y;
        if (abs(dx) > MINIMAP_RADIUS || abs(dy) > MINIMAP_RADIUS) {
            continue;
        }
        float ex = x0 + (float)(dx + MINIMAP_RADIUS) * pixel_size;
        float ey = y0 + (float)(dy + MINIMAP_RADIUS) * pixel_size;

        if (g_entities[i].is_tombstone) {
            /* Tombstone: pulsing purple */
            float tomb_pulse = 0.6f + 0.4f * sinf((float)glfwGetTime() * 2.0f);
            glColor4f(0.7f * tomb_pulse, 0.2f, 0.9f * tomb_pulse, 0.95f);
            /* Cross symbol (horizontal + vertical lines) */
            glLineWidth(1.5f);
            glBegin(GL_LINES);
                glVertex2f(ex - dot * 1.2f, ey);
                glVertex2f(ex + dot * 1.2f, ey);
                glVertex2f(ex, ey - dot * 1.2f);
                glVertex2f(ex, ey + dot * 1.2f);
            glEnd();
            glLineWidth(1.0f);
        } else if (g_entities[i].is_merchant) {
            glColor4f(1.0f, 0.85f, 0.0f, 0.9f);
            glBegin(GL_QUADS);
                glVertex2f(ex - dot * 0.8f, ey - dot * 0.8f);
                glVertex2f(ex + dot * 0.8f, ey - dot * 0.8f);
                glVertex2f(ex + dot * 0.8f, ey + dot * 0.8f);
                glVertex2f(ex - dot * 0.8f, ey + dot * 0.8f);
            glEnd();
        } else {
            glColor4f(1.0f, 0.2f, 0.2f, 0.9f);
            glBegin(GL_QUADS);
                glVertex2f(ex - dot * 0.8f, ey - dot * 0.8f);
                glVertex2f(ex + dot * 0.8f, ey - dot * 0.8f);
                glVertex2f(ex + dot * 0.8f, ey + dot * 0.8f);
                glVertex2f(ex - dot * 0.8f, ey + dot * 0.8f);
            glEnd();
        }
    }

    glDisable(GL_BLEND);

    /* "RADAR" label */
    draw_text(x0, y0 + size + 6.0f, "RADAR", 1.5f, 0.1f, 0.5f, 0.1f);

    /*Restore state*/
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}


static void draw_player_names_gl(int width, int height, double mv[16], double pj[16], int vp[4]) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, width, height, 0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    pthread_mutex_lock(&g_state_mutex);
    int px = g_my_x;
    int py = g_my_y;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_entities[i].active && g_entities[i].is_player && g_entities[i].id != g_my_entity_id && g_entities[i].floor_id == g_my_floor) {
            if (g_entities[i].username[0] != '\0') {
                float ex = (float)(g_entities[i].x - px);
                float ez = (float)(g_entities[i].y - py);
                double winX, winY, winZ;
                if (gluProject(ex, 1.2, ez, mv, pj, vp, &winX, &winY, &winZ)) {
                    if (winZ >= 0.0 && winZ < 1.0) { // in front of camera
                        float len = strlen(g_entities[i].username) * 5.0f * 1.5f;
                        draw_text((float)winX - len / 2.0f, (float)(height - winY), g_entities[i].username, 1.5f, 0.4f, 1.0f, 0.4f);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_state_mutex);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

void render_gl_start(void) {
    if (!glfwInit()) {
        printf("[ERROR] Failed to initialize GLFW. (Are you in a graphical environment?)\n");
        return;
    }
    GLFWwindow* window = glfwCreateWindow(1920, 1080, "dragongl - RPG Client", NULL, NULL);
    if (!window) { 
        printf("[ERROR] Failed to create GLFW window.\n");
        glfwTerminate(); 
        return; 
    }
    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, key_callback);
    glfwSetCursorPosCallback(window, cursor_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE); //Essential for climbing cubes without breaking the light
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    
    float light_ambient[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    float light_diffuse[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 1.0f);
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.0f);
    glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0.0f);

    /*Delta-time for smooth interpolation*/
    double g_last_frame = glfwGetTime();

    while (!glfwWindowShouldClose(window) && g_running) {
        double g_now = glfwGetTime();
        float dt = (float)(g_now - g_last_frame);
        if (dt > 0.1f) dt = 0.1f; /*clamp to prevent jumps on lag spike*/
        g_last_frame = g_now;

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        if (width < 300) width = 300; //Minimum security
        
        if (bg_flash > 0.0f) { glClearColor(bg_flash, 0.0f, 0.0f, 1.0f); bg_flash -= 0.05f; }
        else glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 1. RENDER 3D
        glViewport(0, 0, width, height);
        
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(45.0, (double)width/height, 0.5, 500.0);
        
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        float rad_pitch = camera_pitch * (float)M_PI / 180.0f;
        float rad_yaw = camera_yaw * (float)M_PI / 180.0f;
        float cx = camera_dist * cosf(rad_pitch) * sinf(rad_yaw);
        float cy = camera_dist * sinf(rad_pitch);
        float cz = camera_dist * cosf(rad_pitch) * cosf(rad_yaw);
        gluLookAt(cx, cy, cz, 0, 0, 0, 0, 1, 0);

        //Light localized on the player (above the head)
        float light_pos[] = { 0.0f, 2.0f, 0.0f, 1.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

        pthread_mutex_lock(&g_state_mutex);
        int px = g_my_x, py = g_my_y;

        /*Update interpolated player position*/
        lerp_update(&g_player_lerp, 0.0f, 0.0f, dt);
        
        particles_update(dt);
        
        /*(The player is always in the center — the world moves around him)*/
        int vr = g_vision_radius;

        for (int y = py-vr; y <= py+vr; y++) {
            for (int x = px-vr; x <= px+vr; x++) {
                if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) continue;
                TileType t = g_local_map[y][x];
                if (t == VOXEL_ROCK) continue;
                float d = sqrtf((float)((x-px)*(x-px) + (y-py)*(y-py)));
                if (d > (float)vr) continue;
                
                float anim_time = (float)glfwGetTime();
                float liquid_y = -0.1f + sinf(anim_time * 2.0f + (float)x * 0.5f + (float)y * 0.5f) * 0.08f;
                float pulse = 0.8f + sinf(anim_time * 4.0f + (float)(x+y)) * 0.2f;

                /*Soft Fog of War: Gradually fades between 70% and 100% of view range*/
                float t_fog = (d - (float)vr * 0.7f) / ((float)vr * 0.3f);
                if (t_fog < 0.0f) t_fog = 0.0f;
                if (t_fog > 1.0f) t_fog = 1.0f;
                float fog = 1.0f - (t_fog * t_fog * (3.0f - 2.0f * t_fog)); /* manual smoothstep */

                /* Subtle vignette */
                float vignette = 1.0f - (d / ((float)vr * 1.2f)) * 0.15f;
                if (vignette < 0.5f) vignette = 0.5f;
                if (vignette > 1.0f) vignette = 1.0f;
                
                float total_fade = fog * vignette;

                #define SET_COLOR(r, g, b) glColor3f((r)*total_fade, (g)*total_fade, (b)*total_fade)

                switch(t) {
                    case VOXEL_WALL: SET_COLOR(0.6f, 0.6f, 0.6f); draw_cube((float)(x-px), 0.5f, (float)(y-py), 1, 1, 1); break;
                    case VOXEL_OBSIDIAN: SET_COLOR(0.1f, 0.05f, 0.2f); draw_cube((float)(x-px), 0.5f, (float)(y-py), 1, 1, 1); break;
                    case VOXEL_GOLD_VEIN: SET_COLOR(0.8f, 0.7f, 0.1f); draw_cube((float)(x-px), 0.5f, (float)(y-py), 1, 1, 1); break;
                    case VOXEL_CRYSTAL_BLUE:   SET_COLOR(0.3f * pulse, 0.7f * pulse, 1.0f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_PURPLE: SET_COLOR(0.8f * pulse, 0.2f * pulse, 1.0f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_RED:    SET_COLOR(1.0f * pulse, 0.1f,          0.1f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_GREEN:  SET_COLOR(0.1f,         1.0f * pulse,  0.2f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_YELLOW: SET_COLOR(1.0f * pulse, 0.9f * pulse,  0.1f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_ORANGE: SET_COLOR(1.0f * pulse, 0.5f * pulse,  0.0f); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_CYAN:   SET_COLOR(0.0f,         0.9f * pulse,  1.0f * pulse); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_CRYSTAL_WHITE:  SET_COLOR(0.9f * pulse, 0.95f* pulse,  1.0f * pulse); draw_cube((float)(x-px), 0.0f, (float)(y-py), 0.8f, 1.6f, 0.8f); break;
                    case VOXEL_MUSHROOM_GLOW: SET_COLOR(0.2f, 1.0f * pulse, 0.5f); draw_cube((float)(x-px), -0.6f, (float)(y-py), 0.6f, 0.8f, 0.6f); break;
                    case VOXEL_DOOR: SET_COLOR(0.5f, 0.3f, 0.1f); draw_cube((float)(x-px), 0.4f, (float)(y-py), 0.9f, 0.8f, 0.9f); break;
                    case VOXEL_STAIRS_DOWN: SET_COLOR(0.9f, 0.9f, 0.0f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_STAIRS_UP:   SET_COLOR(0.0f, 0.9f, 0.9f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_GRASS: SET_COLOR(0.1f, 0.5f, 0.1f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_WATER: SET_COLOR(0.1f, 0.4f, 0.8f); draw_cube((float)(x-px), liquid_y - 0.4f, (float)(y-py), 1.0f, 0.1f, 1.0f); break;
                    case VOXEL_LAVA: SET_COLOR(1.0f, 0.3f * pulse, 0.0f); draw_cube((float)(x-px), liquid_y - 0.4f, (float)(y-py), 1.0f, 0.1f, 1.0f); break;
                    case VOXEL_WOOD: SET_COLOR(0.4f, 0.3f, 0.2f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_COBBLE: SET_COLOR(0.3f, 0.3f, 0.3f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_ICE: SET_COLOR(0.6f, 0.8f, 1.0f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_SAND: SET_COLOR(0.8f, 0.7f, 0.4f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_ASH: SET_COLOR(0.25f, 0.25f, 0.25f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_MUD: SET_COLOR(0.3f, 0.2f, 0.1f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_MARBLE: SET_COLOR(0.9f, 0.9f, 0.9f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_TRAP:   SET_COLOR(0.8f, 0.2f, 0.1f); draw_tile((float)(x-px), (float)(y-py)); break;
                    case VOXEL_FLOOR: SET_COLOR(0.2f, 0.2f, 0.2f); draw_tile((float)(x-px), (float)(y-py)); break;
                    default: break;
                }
                
                #undef SET_COLOR
            }
        }
        glColor3f(1.0f, 0.1f, 0.1f); draw_cube(0.0f, 0.6f, 0.0f, 0.6f, 1.2f, 0.6f);

        //--- AR COMPASS (3D Overlay) ---
        //Draw a directional dial around the player
        glDisable(GL_LIGHTING);
        glLineWidth(2.0f);
        float r_comp = 2.5f; //Compass radius
        glBegin(GL_LINES);
            // North (dark green) - negative Z axis (Y-1 in the game)
            glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(0.0f, 0.05f, -r_comp); glVertex3f(0.0f, 0.05f, -r_comp + 0.5f);
            // South (gray)
            glColor3f(0.5f, 0.5f, 0.5f); glVertex3f(0.0f, 0.05f, r_comp); glVertex3f(0.0f, 0.05f, r_comp - 0.5f);
            // East (gray)
            glColor3f(0.5f, 0.5f, 0.5f); glVertex3f(r_comp, 0.05f, 0.0f); glVertex3f(r_comp - 0.5f, 0.05f, 0.0f);
            // West (gray)
            glColor3f(0.5f, 0.5f, 0.5f); glVertex3f(-r_comp, 0.05f, 0.0f); glVertex3f(-r_comp + 0.5f, 0.05f, 0.0f);
        glEnd();

        // 3D text labels (billboard-ish)
        //Do we use draw_text by temporarily resetting the matrix to project 3D text?
        //Use a simpler approach: project into the HUD.
        //But for true AR, we use lines to compose the letters.
        glBegin(GL_LINES);
            // N (North)
            glColor3f(0.0f, 1.0f, 0.0f);
            glVertex3f(-0.1f, 0.1f, -r_comp-0.3f); glVertex3f(-0.1f, 0.1f, -r_comp-0.7f);
            glVertex3f(-0.1f, 0.1f, -r_comp-0.3f); glVertex3f(0.1f, 0.1f, -r_comp-0.7f);
            glVertex3f(0.1f, 0.1f, -r_comp-0.3f); glVertex3f(0.1f, 0.1f, -r_comp-0.7f);
            // S (South)
            glColor3f(0.7f, 0.7f, 0.7f);
            glVertex3f(-0.1f, 0.1f, r_comp+0.3f); glVertex3f(0.1f, 0.1f, r_comp+0.3f);
            glVertex3f(-0.1f, 0.1f, r_comp+0.3f); glVertex3f(-0.1f, 0.1f, r_comp+0.5f);
            glVertex3f(-0.1f, 0.1f, r_comp+0.5f); glVertex3f(0.1f, 0.1f, r_comp+0.5f);
            glVertex3f(0.1f, 0.1f, r_comp+0.5f); glVertex3f(0.1f, 0.1f, r_comp+0.7f);
            glVertex3f(-0.1f, 0.1f, r_comp+0.7f); glVertex3f(0.1f, 0.1f, r_comp+0.7f);
            // E (East)
            glVertex3f(r_comp+0.3f, 0.1f, -0.1f); glVertex3f(r_comp+0.3f, 0.1f, 0.1f);
            glVertex3f(r_comp+0.3f, 0.1f, -0.1f); glVertex3f(r_comp+0.7f, 0.1f, -0.1f);
            glVertex3f(r_comp+0.5f, 0.1f, -0.1f); glVertex3f(r_comp+0.5f, 0.1f, 0.1f);
            glVertex3f(r_comp+0.7f, 0.1f, -0.1f); glVertex3f(r_comp+0.7f, 0.1f, 0.1f);
            // W (West)
            glVertex3f(-r_comp-0.3f, 0.1f, -0.1f); glVertex3f(-r_comp-0.7f, 0.1f, -0.1f);
            glVertex3f(-r_comp-0.3f, 0.1f, 0.1f); glVertex3f(-r_comp-0.7f, 0.1f, 0.1f);
            glVertex3f(-r_comp-0.7f, 0.1f, -0.1f); glVertex3f(-r_comp-0.5f, 0.1f, 0.0f);
            glVertex3f(-r_comp-0.5f, 0.1f, 0.0f); glVertex3f(-r_comp-0.7f, 0.1f, 0.1f);
        glEnd();
        glEnable(GL_LIGHTING);
        glLineWidth(1.0f);

        //Rendering other NPCs/Monsters (with position interpolation)
        for(int i=0; i<MAX_NPCS; i++) {
            if (g_entities[i].active && g_entities[i].id != g_my_entity_id) {
                /*Calculate targets in world coordinates relative to the player*/
                float tgt_ex = (float)(g_entities[i].x - px);
                float tgt_ez = (float)(g_entities[i].y - py);

                /* Update interpolated position for this entity */
                lerp_update(&g_entity_lerp[i], tgt_ex, tgt_ez, dt);

                float ex = g_entity_lerp[i].cur_x;
                float ez = g_entity_lerp[i].cur_z;

                if (fabs(ex) < (float)vr + 1.0f && fabs(ez) < (float)vr + 1.0f) {
                    float d = sqrtf(ex*ex + ez*ez);
                    if (d <= (float)vr) {
                        float t_fog = (d - (float)vr * 0.7f) / ((float)vr * 0.3f);
                        if (t_fog < 0.0f) t_fog = 0.0f;
                        if (t_fog > 1.0f) t_fog = 1.0f;
                        float fog = 1.0f - (t_fog * t_fog * (3.0f - 2.0f * t_fog));
                        float vignette = 1.0f - (d / ((float)vr * 1.2f)) * 0.15f;
                        if (vignette < 0.5f) vignette = 0.5f;
                        if (vignette > 1.0f) vignette = 1.0f;
                        float total_fade = fog * vignette;

                        /*Color based on type*/
                        float r = 0.4f, g = 0.4f, b = 1.0f;
                        if (g_entities[i].is_tombstone) {

                            /* Tombstone: dark gray slab, no bob */
                            float tp = 0.5f + 0.2f * sinf((float)glfwGetTime() * 1.5f);
                            glColor3f(0.55f * tp * total_fade,
                                      0.35f * tp * total_fade,
                                      0.70f * tp * total_fade);
                            /* Flat slab (reduced height, wide) */
                            draw_cube(ex, 0.05f, ez, 0.65f, 0.12f, 0.12f);
                            /*Vertical arm of the cross*/
                            glColor3f(0.90f * total_fade, 0.90f * total_fade, 0.90f * total_fade);
                            draw_cube(ex, 0.35f, ez, 0.06f, 0.55f, 0.06f);
                            /*Horizontal arm of the cross*/
                            draw_cube(ex, 0.55f, ez, 0.30f, 0.06f, 0.06f);
                        } else if (g_entities[i].is_player) {
                            r = 0.2f;
                            g = 0.8f;
                            b = 0.2f;
                            glColor3f(r * total_fade, g * total_fade, b * total_fade);
                            draw_pyramid(ex, 0.5f, ez, 0.7f, 1.0f, 0.7f);
                        } else if (g_entities[i].is_merchant) {
                            r = 1.0f;
                            g = 0.8f;
                            b = 0.0f;
                            glColor3f(r * total_fade, g * total_fade, b * total_fade);
                            draw_cube(ex, 0.5f, ez, 0.7f, 1.0f, 0.7f);
                        } else if (g_entities[i].id < 10) {
                            r = 1.0f;
                            g = 0.3f;
                            b = 0.3f;
                            glColor3f(r * total_fade, g * total_fade, b * total_fade);
                            draw_cube(ex, 0.5f, ez, 0.7f, 1.0f, 0.7f);
                        } else {
                            r = 0.4f;
                            g = 0.4f;
                            b = 1.0f;
                            glColor3f(r * total_fade, g * total_fade, b * total_fade);
                            draw_cube(ex, 0.5f, ez, 0.7f, 1.0f, 0.7f);
                        }
                    }
                }
            } else if (!g_entities[i].active) {
                /* Reset lerp for entities no longer active */
                g_entity_lerp[i].initialized = false;
            }
        }
        double modelview[16], projection[16];
        int viewport[4];
        glGetDoublev(GL_MODELVIEW_MATRIX, modelview);
        glGetDoublev(GL_PROJECTION_MATRIX, projection);
        glGetIntegerv(GL_VIEWPORT, viewport);
        
        draw_particles();
        pthread_mutex_unlock(&g_state_mutex);

        // 2. RENDER HUD
        glViewport(0, 0, width, height);
        render_gl_hud(width, height);
        draw_player_names_gl(width, height, modelview, projection, viewport);

        // 3. FLOATING COMBAT TEXT
        fct_update(dt);
        draw_floating_combat_text(width, height);

        // 4. MINIMAP RADAR
        minimap_update(px, py, g_local_map, vr);
        draw_minimap_gl(width, height);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
        
        /*Polling for key held: Send every g_movement_cooldown seconds.
         * last_m starts from the current time to not send immediately to the first frame
         * (the GLFW_PRESS in key_callback has already sent the first packet).*/
        static double last_m = -1.0;
        double n = glfwGetTime();
        if (last_m < 0.0) {
            last_m = n; /*First initialization: wait for a full cooldown*/
        }
        if (n - last_m >= (double)g_movement_cooldown) {
            bool mov = false;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                client_send_move(0, -1);
                mov = true;
            } else if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                client_send_move(0, 1);
                mov = true;
            } else if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                client_send_move(-1, 0);
                mov = true;
            } else if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                client_send_move(1, 0);
                mov = true;
            }
            if (mov) {
                last_m = n;
            }
        }
    }
    g_running = false;
    glfwTerminate();
}
