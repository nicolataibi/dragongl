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
 * hud_draw.c - Rectangles, text and panels of the HUD (hud_draw.h).
 */

#include "hud_draw.h"

#include "font5x7.h"

#include <math.h>
#include <string.h>

/*Dots of the font across one character cell and down one line.*/
#define CELL_DOTS 6
#define LINE_DOTS 10

void hud_begin(Hud *h, VkVertex *v, uint32_t max, float fb_w, float fb_h, float scale,
               bool srgb_target) {
    memset(h, 0, sizeof(*h));
    h->v = v;
    h->max = max;
    h->scale = scale;
    h->linear = srgb_target;
    h->w = fb_w / scale;
    h->h = fb_h / scale;
    /*A dot of the font is a whole number of device pixels.*/
    float device = floorf(2.0f * scale + 0.5f);
    if (device < 1.0f) {
        device = 1.0f;
    }
    h->dot = device / scale;
    h->cell_w = CELL_DOTS * h->dot;
    h->line_h = LINE_DOTS * h->dot;
}

static void push(Hud *h, float x, float y, Rgb c, float a) {
    VkVertex *v = &h->v[h->count++];
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = 0.0f;
    v->color[0] = c.r;
    v->color[1] = c.g;
    v->color[2] = c.b;
    v->color[3] = a;
    v->normal[0] = 0.0f;
    v->normal[1] = 0.0f;
    v->normal[2] = 1.0f;
}

void hud_quad(Hud *h, float x, float y, float w, float ht, Rgb c, float alpha) {
    if (h->count + 6 > h->max || w <= 0.0f || ht <= 0.0f) {
        return;
    }
    if (h->linear) {
        c.r = (float)srgb_to_linear(c.r);
        c.g = (float)srgb_to_linear(c.g);
        c.b = (float)srgb_to_linear(c.b);
    }
    /*Edges on whole device pixels: neighbouring rectangles never leave a
     * seam or overlap.*/
    float x0 = floorf(x * h->scale + 0.5f), y0 = floorf(y * h->scale + 0.5f);
    float x1 = floorf((x + w) * h->scale + 0.5f), y1 = floorf((y + ht) * h->scale + 0.5f);
    push(h, x0, y0, c, alpha);
    push(h, x1, y0, c, alpha);
    push(h, x0, y1, c, alpha);
    push(h, x1, y0, c, alpha);
    push(h, x1, y1, c, alpha);
    push(h, x0, y1, c, alpha);
}

float hud_text_width(const Hud *h, const char *s, int size) {
    return (float)strlen(s) * h->cell_w * (float)size;
}

/*One character: each row of the glyph as runs of lit dots, one rectangle
 * per run (a third of the triangles of one rectangle per dot).*/
static void glyph(Hud *h, float x, float y, unsigned char ch, Rgb c, float dot) {
    const uint8_t *cols = font5x7[ch - 32];
    for (int row = 0; row < 7; row++) {
        int run = -1;
        for (int col = 0; col <= 5; col++) {
            bool lit = col < 5 && (cols[col] & (1 << row));
            if (lit && run < 0) {
                run = col;
            } else if (!lit && run >= 0) {
                hud_quad(h, x + (float)run * dot, y + (float)row * dot, (float)(col - run) * dot,
                         dot, c, 1.0f);
                run = -1;
            }
        }
    }
}

float hud_text(Hud *h, float x, float y, const char *s, Rgb c, int size) {
    float dot = h->dot * (float)size;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch > 32 && ch <= 127) {
            glyph(h, x, y, ch, c, dot);
        }
        x += CELL_DOTS * dot;
    }
    return x;
}

void hud_panel(Hud *h, float x, float y, float w, float ht) {
    const Rgb edge = {0.30f, 0.34f, 0.40f};
    float t = h->dot * 0.5f;
    hud_quad(h, x, y, w, ht, HUD_PANEL, HUD_PANEL_ALPHA);
    hud_quad(h, x, y, w, t, edge, 1.0f);
    hud_quad(h, x, y + ht - t, w, t, edge, 1.0f);
    hud_quad(h, x, y + t, t, ht - 2.0f * t, edge, 1.0f);
    hud_quad(h, x + w - t, y + t, t, ht - 2.0f * t, edge, 1.0f);
}

void hud_label(Hud *h, float cx, float y, const char *s, Rgb c, int size) {
    float w = hud_text_width(h, s, size);
    float pad = h->dot * 1.5f;
    float x = cx - w * 0.5f;
    hud_quad(h, x - pad, y - pad, w + 2.0f * pad - h->dot * (float)size,
             7.0f * h->dot * (float)size + 2.0f * pad, HUD_PANEL, HUD_PANEL_ALPHA);
    hud_text(h, x, y, s, c, size);
}
