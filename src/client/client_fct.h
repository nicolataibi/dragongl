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

/**
 * client_fct.h — Floating Combat Text (FCT) System
 *
 * Module independent of the graphical backend (OpenGL/Vulkan).
 * Maintains a pre-allocated array of floating texts (damage numbers,
 * cures, critical messages) that renderers read and draw.
 *
 * Performance: Static array, no dynamic allocation.
 * Every frame, the renderer calls fct_update() and then iterates over g_fct[].*/

#ifndef CLIENT_FCT_H
#define CLIENT_FCT_H

#include <stdbool.h>

#define FCT_MAX_ENTRIES 64
#define FCT_TEXT_LEN    16

/*Floating text type (determines color and behavior)*/
typedef enum {
    FCT_DAMAGE,     /*Red: Damage suffered by the player*/
    FCT_DAMAGE_OUT, /*Orange: Damage dealt by the player to an enemy*/
    FCT_HEAL,       /*Green: Healing received*/
    FCT_CRITICAL,   /*Yellow: Critical hit*/
    FCT_XP,         /*Cyan: XP gained*/
    FCT_GOLD,       /*Gold: Coins collected*/
    FCT_MISS,       /*Gray: Missed hit*/
    FCT_LEVELUP     /*White: Level up*/
} FctType;

/*Single floating text*/
typedef struct {
    bool   active;
    FctType type;
    char   text[FCT_TEXT_LEN]; /* e.g. "-15", "+5", "CRIT!", "MISS"          */
    float  world_x;            /*world position (relative to player)*/
    float  world_z;            /*world position (relative to player)*/
    float  offset_y;           /*current height (rises over time)*/
    float  alpha;              /* current opacity (1.0 → 0.0)              */
    float  life;               /*remaining time in seconds*/
    float  max_life;           /*total duration (to calculate alpha)*/
    float  scale;              /*text scale*/
} FloatingText;

/*Global array of FCTs — read by renderers*/
extern FloatingText g_fct[FCT_MAX_ENTRIES];

/**
 * fct_spawn — Generates new floating text.
 * @param type Type (damage, heal, critical, etc.)
 * @param wx World X position (relative to player)
 * @param wz World Z position (relative to player)
 * @param text String to display (e.g. "-12", "+5")*/
void fct_spawn(FctType type, float wx, float wz, const char *text);

/**
 * fct_update — Update all active texts (call every frame).
 * @param dt Delta-time in seconds from the previous frame.*/
void fct_update(float dt);

/**
 * fct_parse_log — Parse a server log line to generate
 * automatically appropriate FCTs.
 * @param log_line The text line received from the server.*/
void fct_parse_log(const char *log_line);

#endif /* CLIENT_FCT_H */
