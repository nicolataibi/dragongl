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
 * hud_draw.h - Drawing primitives of the HUD: rectangles, text and
 * panels, as triangles in the HUD vertex buffer of the frame.
 *
 * Coordinates are virtual pixels: the framebuffer divided by the HUD
 * scale (client_config.h), so a layout written once fits every monitor.
 * Text is the 5x7 bitmap font; each of its dots is a whole number of
 * device pixels (2 at scale 1), so it stays sharp at any scale.
 */

#ifndef HUD_DRAW_H
#define HUD_DRAW_H

#include "hud_theme.h"
#include "render_vk.h"

#include <stdint.h>

typedef struct {
    VkVertex *v;
    uint32_t count, max;
    float scale;  /*device pixels per virtual pixel*/
    float w, h;   /*virtual size of the framebuffer*/
    float dot;    /*virtual size of one dot of the font*/
    float cell_w; /*advance of one character*/
    float line_h; /*height of one line of text, leading included*/
    bool linear;  /*the target is sRGB: colours are written in linear light*/
} Hud;

/*srgb_target: the swapchain format is sRGB, so the GPU encodes what is
 * written; the colours of hud_theme.h are then converted to linear light
 * and reach the screen as they are defined.*/
void hud_begin(Hud *h, VkVertex *v, uint32_t max, float fb_w, float fb_h, float scale,
               bool srgb_target);

void hud_quad(Hud *h, float x, float y, float w, float ht, Rgb c, float alpha);
/*Text with its top left corner at (x, y); returns the x after the last
 * character. size: 1 for ordinary text, 2 for double-size.*/
float hud_text(Hud *h, float x, float y, const char *s, Rgb c, int size);
float hud_text_width(const Hud *h, const char *s, int size);
/*A dark translucent rectangle with a thin border: what text sits on
 * (hud_theme.h: the contrast of every text colour is checked on it).*/
void hud_panel(Hud *h, float x, float y, float w, float ht);
/*Text centred on cx with its own small panel: for labels drawn over the
 * 3D scene.*/
void hud_label(Hud *h, float cx, float y, const char *s, Rgb c, int size);

#endif /* HUD_DRAW_H */
