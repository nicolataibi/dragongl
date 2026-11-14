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
 * hud_theme.h - The colours of the HUD and the contrast they must keep.
 *
 * Every text of the HUD is drawn in one of these colours on a panel of
 * HUD_PANEL colour and HUD_PANEL_ALPHA opacity. tests/test_client_ui.c
 * checks that each of them keeps a contrast of at least 4.5:1 (WCAG 2.1,
 * 1.4.3) on the panel, whatever is behind it, from black to white.
 */

#ifndef HUD_THEME_H
#define HUD_THEME_H

typedef enum {
    HUD_TEXT,   /*ordinary text*/
    HUD_DIM,    /*labels, empty slots*/
    HUD_TITLE,  /*panel titles*/
    HUD_GOOD,   /*healthy, heal*/
    HUD_WARN,   /*hungry, wounded*/
    HUD_DANGER, /*damage taken, critical state*/
    HUD_INFO,   /*experience, system messages*/
    HUD_GOLD,
    HUD_MAGIC,      /*spell slots, conditions*/
    HUD_DAMAGE_OUT, /*damage dealt*/
    HUD_NAME,       /*other characters*/
    HUD_COLOR_COUNT
} HudColor;

typedef struct {
    float r, g, b;
} Rgb;

extern const Rgb HUD_PANEL;
#define HUD_PANEL_ALPHA 0.94f
/*Smallest contrast of text on a panel (WCAG AA for normal text).*/
#define HUD_MIN_CONTRAST 4.5

Rgb hud_color(HudColor c);
const char *hud_color_name(HudColor c);

/*Relative luminance and contrast ratio of two sRGB colours (WCAG 2.1).*/
double rgb_luminance(Rgb c);
double rgb_contrast(Rgb a, Rgb b);
/*An sRGB component in linear light.*/
double srgb_to_linear(double v);
/*The panel as it looks over a backdrop of that colour (blended in linear
 * light, as the GPU does on an sRGB swapchain).*/
Rgb hud_panel_over(Rgb backdrop);

/*Colour of a line of the message log, from its tag: "[COMBAT] ..." and
 * the like.*/
HudColor hud_message_color(const char *line);

#endif /* HUD_THEME_H */
