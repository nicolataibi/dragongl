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
 * hud_theme.c - Colours of the HUD (hud_theme.h).
 */

#include "hud_theme.h"

#include <math.h>
#include <string.h>

const Rgb HUD_PANEL = {0.03f, 0.04f, 0.06f};

static const struct {
    const char *name;
    Rgb rgb;
} k_colors[HUD_COLOR_COUNT] = {
    [HUD_TEXT] = {"text", {0.93f, 0.93f, 0.90f}},
    [HUD_DIM] = {"dim", {0.72f, 0.74f, 0.78f}},
    [HUD_TITLE] = {"title", {0.55f, 0.95f, 0.55f}},
    [HUD_GOOD] = {"good", {0.45f, 0.95f, 0.45f}},
    [HUD_WARN] = {"warn", {1.00f, 0.82f, 0.25f}},
    [HUD_DANGER] = {"danger", {1.00f, 0.60f, 0.56f}},
    [HUD_INFO] = {"info", {0.50f, 0.85f, 1.00f}},
    [HUD_GOLD] = {"gold", {1.00f, 0.84f, 0.30f}},
    [HUD_MAGIC] = {"magic", {0.82f, 0.70f, 1.00f}},
    [HUD_DAMAGE_OUT] = {"damage_out", {1.00f, 0.68f, 0.30f}},
    [HUD_NAME] = {"name", {0.60f, 1.00f, 0.60f}},
};

Rgb hud_color(HudColor c) {
    return k_colors[c >= 0 && c < HUD_COLOR_COUNT ? c : HUD_TEXT].rgb;
}

const char *hud_color_name(HudColor c) {
    return c >= 0 && c < HUD_COLOR_COUNT ? k_colors[c].name : "";
}

static double linear(double v) {
    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

double rgb_luminance(Rgb c) {
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

double rgb_contrast(Rgb a, Rgb b) {
    double la = rgb_luminance(a), lb = rgb_luminance(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

double srgb_to_linear(double v) {
    return linear(v);
}

static double to_srgb(double v) {
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/*The GPU blends in linear light (the swapchain is sRGB): so does this.*/
static float over(float panel, float backdrop) {
    double lin = linear(panel) * HUD_PANEL_ALPHA + linear(backdrop) * (1.0 - HUD_PANEL_ALPHA);
    return (float)to_srgb(lin);
}

Rgb hud_panel_over(Rgb backdrop) {
    Rgb out = {over(HUD_PANEL.r, backdrop.r), over(HUD_PANEL.g, backdrop.g),
               over(HUD_PANEL.b, backdrop.b)};
    return out;
}

HudColor hud_message_color(const char *line) {
    static const struct {
        const char *tag;
        HudColor color;
    } k_tags[] = {
        {"[COMBAT]", HUD_DAMAGE_OUT}, {"[DANGER]", HUD_DANGER}, {"[DEATH]", HUD_DANGER},
        {"[ERROR]", HUD_DANGER},      {"[SYSTEM]", HUD_INFO},   {"[DM]", HUD_INFO},
        {"[HELP]", HUD_INFO},         {"[TIP]", HUD_INFO},      {"[WHO]", HUD_INFO},
        {"[LOOT]", HUD_GOLD},         {"[TRADE]", HUD_GOLD},    {"[STORE]", HUD_GOLD},
        {"[VICTORY]", HUD_GOLD},      {"[MAGIC]", HUD_MAGIC},   {"[RITUAL]", HUD_MAGIC},
        {"[CHAT]", HUD_NAME},         {"[PARTY]", HUD_NAME},    {"[MSG]", HUD_NAME},
        {"[WARNING]", HUD_WARN},      {"[EVENT]", HUD_WARN},    {"[STATUS]", HUD_WARN},
    };
    for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); i++) {
        if (strncmp(line, k_tags[i].tag, strlen(k_tags[i].tag)) == 0) {
            return k_tags[i].color;
        }
    }
    return HUD_TEXT;
}
