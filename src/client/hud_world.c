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
 * hud_world.c - Labels drawn over the 3D scene: the names of the other
 * characters, the combat numbers and the compass (hud.h).
 */

#include "hud.h"

#include "client_fct.h"
#include "client_math.h"
#include "vk_scene.h"

#include <string.h>

/*A world position on the HUD, in virtual pixels.*/
static bool on_screen(const Hud *h, const float mvp[16], float x, float y, float z, float *sx,
                      float *sy) {
    bool visible = false;
    project_point(mvp, x, y, z, h->w, h->h, sx, sy, &visible);
    return visible;
}

static void names(Hud *h, const float mvp[16], const FrameSnapshot *snap) {
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &snap->entities[i];
        if (!e->active || !e->is_player || e->id == snap->my_entity_id ||
            e->floor_id != snap->my_floor || e->username[0] == '\0') {
            continue;
        }
        float ex = (float)e->x, ez = (float)e->y, sx = 0.0f, sy = 0.0f;
        (void)vk_scene_entity_pos(i, &ex, &ez);
        if (on_screen(h, mvp, ex, 1.2f, ez, &sx, &sy)) {
            hud_label(h, sx, sy, e->username, hud_color(HUD_NAME), 1);
        }
    }
}

static HudColor fct_color(FctType t) {
    switch (t) {
    case FCT_DAMAGE:
        return HUD_DANGER;
    case FCT_DAMAGE_OUT:
        return HUD_DAMAGE_OUT;
    case FCT_HEAL:
        return HUD_GOOD;
    case FCT_CRITICAL:
        return HUD_WARN;
    case FCT_XP:
        return HUD_INFO;
    case FCT_GOLD:
        return HUD_GOLD;
    case FCT_MISS:
        return HUD_DIM;
    case FCT_LEVELUP:
        return HUD_TEXT;
    }
    return HUD_TEXT;
}

/*Combat numbers rise from where they happened; with reduce_motion they
 * stay where they appeared.*/
static void combat_text(Hud *h, const float mvp[16], float px, float pz, bool reduce_motion) {
    pthread_mutex_lock(&g_fct_mutex);
    for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
        const FloatingText *f = &g_fct[i];
        if (!f->active) {
            continue;
        }
        float sx = 0.0f, sy = 0.0f;
        float height = reduce_motion ? 1.6f : f->offset_y;
        if (on_screen(h, mvp, px + f->world_x, height, pz + f->world_z, &sx, &sy)) {
            hud_label(h, sx, sy, f->text, hud_color(fct_color(f->type)), f->scale >= 1.5f ? 2 : 1);
        }
    }
    pthread_mutex_unlock(&g_fct_mutex);
}

/*N, S, E, W on the ground, three tiles from the character.*/
static void compass(Hud *h, const float mvp[16], float px, float pz) {
    static const struct {
        const char *letter;
        float dx, dz;
        HudColor color;
    } points[4] = {{"N", 0.0f, -3.0f, HUD_DANGER},
                   {"S", 0.0f, 3.0f, HUD_TEXT},
                   {"E", 3.0f, 0.0f, HUD_TEXT},
                   {"W", -3.0f, 0.0f, HUD_TEXT}};
    for (int i = 0; i < 4; i++) {
        float sx = 0.0f, sy = 0.0f;
        if (on_screen(h, mvp, px + points[i].dx, 0.5f, pz + points[i].dz, &sx, &sy)) {
            hud_label(h, sx, sy - 4.0f * h->dot, points[i].letter, hud_color(points[i].color), 1);
        }
    }
}

void hud_world_labels(Hud *h, const float mvp[16], const FrameSnapshot *snap, float px, float pz,
                      bool reduce_motion) {
    compass(h, mvp, px, pz);
    names(h, mvp, snap);
    combat_text(h, mvp, px, pz, reduce_motion);
}
