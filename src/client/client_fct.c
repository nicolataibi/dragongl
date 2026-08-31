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
 * client_fct.c — Floating Combat Text (FCT) System — Implementation
 *
 * Handles generation, updating and automatic parsing
 * Floating damage/heal numbers on screen.
 *
 * Performance: No dynamic allocation. Static array O(N)
 * with N = FCT_MAX_ENTRIES (64). Linear scan with early-exit.*/

#include "client_fct.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/*Pre-allocated global array — zero-initialized (all inactive)*/
FloatingText g_fct[FCT_MAX_ENTRIES] = {0};

/*----------------------------------------------------------------
 * Animation constants
 * ----------------------------------------------------------------*/
#define FCT_RISE_SPEED   1.8f  /* units/second upward                    */
#define FCT_DRIFT_RANGE  0.3f  /* random horizontal dispersion           */
#define FCT_DEFAULT_LIFE 1.2f  /*standard duration in seconds*/
#define FCT_CRIT_LIFE    1.8f  /*extended runtime for critics*/

/*----------------------------------------------------------------
* fct_spawn — Inserts a new FCT into the first free slot.
 * ----------------------------------------------------------------*/
void fct_spawn(FctType type, float wx, float wz, const char *text) {
    for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
        if (!g_fct[i].active) {
            FloatingText *f = &g_fct[i];
            f->active   = true;
            f->type     = type;
            f->world_x  = wx + ((float)rand() / (float)RAND_MAX - 0.5f) * FCT_DRIFT_RANGE;
            f->world_z  = wz + ((float)rand() / (float)RAND_MAX - 0.5f) * FCT_DRIFT_RANGE;
            f->offset_y = 1.2f;
            f->alpha    = 1.0f;

            /*Duration and scale vary by type*/
            switch (type) {
                case FCT_CRITICAL:
                    f->max_life = FCT_CRIT_LIFE;
                    f->scale    = 3.5f;
                    break;
                case FCT_LEVELUP:
                    f->max_life = 2.5f;
                    f->scale    = 4.0f;
                    break;
                case FCT_XP:
                case FCT_GOLD:
                    f->max_life = 1.5f;
                    f->scale    = 2.0f;
                    break;
                default:
                    f->max_life = FCT_DEFAULT_LIFE;
                    f->scale    = 2.5f;
                    break;
            }
            f->life = f->max_life;

            strncpy(f->text, text, FCT_TEXT_LEN - 1);
            f->text[FCT_TEXT_LEN - 1] = '\0';
            return;
        }
    }
    /* If all slots are full, overwrite the oldest one */
    int oldest = 0;
    float min_life = g_fct[0].life;
    for (int i = 1; i < FCT_MAX_ENTRIES; i++) {
        if (g_fct[i].life < min_life) {
            min_life = g_fct[i].life;
            oldest = i;
        }
    }
    FloatingText *f = &g_fct[oldest];
    f->active   = true;
    f->type     = type;
    f->world_x  = wx + ((float)rand() / (float)RAND_MAX - 0.5f) * FCT_DRIFT_RANGE;
    f->world_z  = wz + ((float)rand() / (float)RAND_MAX - 0.5f) * FCT_DRIFT_RANGE;
    f->offset_y = 1.2f;
    f->alpha    = 1.0f;
    f->max_life = (type == FCT_CRITICAL) ? FCT_CRIT_LIFE : FCT_DEFAULT_LIFE;
    f->life     = f->max_life;
    f->scale    = (type == FCT_CRITICAL) ? 3.5f : 2.5f;
    strncpy(f->text, text, FCT_TEXT_LEN - 1);
    f->text[FCT_TEXT_LEN - 1] = '\0';
}

/* ----------------------------------------------------------------
 * fct_update — Updates position and opacity of every active FCT.
 * ---------------------------------------------------------------- */
void fct_update(float dt) {
    for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
        if (!g_fct[i].active) {
            continue;
        }
        FloatingText *f = &g_fct[i];
        f->life -= dt;
        if (f->life <= 0.0f) {
            f->active = false;
            continue;
        }
        /*The text rises upwards*/
        f->offset_y += FCT_RISE_SPEED * dt;

        /*Fade-out in the last 40% of life*/
        float ratio = f->life / f->max_life;
        if (ratio < 0.4f) {
            f->alpha = ratio / 0.4f;
        }
        else {
            f->alpha = 1.0f;
        }
    }
}

/* ----------------------------------------------------------------
 * fct_parse_log — Parses the messages from the server and generates FCTs.
 *
 * The recognized patterns (as currently sent by the server) are:
 *   ... [+C elem) = N ...] ...             → FCT_DAMAGE_OUT
 *   > BLOOD! A devastating blow ...        → FCT_CRITICAL
 *   [COMBAT] You take N damage!            → FCT_DAMAGE
 *   [COMBAT] ... missed you!               → FCT_MISS (enemy misses)
 *   > Missed... the attack bounces off...  → FCT_MISS (player misses)
 *   [LEVEL] *** YOU HAVE RISE TO LEVEL N *** → FCT_LEVELUP
 *   [SYSTEM] Collect a stack of N gold coins! → FCT_GOLD
 *   [MAGIC] ...: Recover N HP              → FCT_HEAL
 *
 * Coordinates are always (0,0) = on the player, since the world is
 * centered on the player in the renderer.
 * ---------------------------------------------------------------- */
void fct_parse_log(const char *log_line) {
    if (!log_line) {
        return;
    }

    char buf[FCT_TEXT_LEN];

    /*--- Damage dealt to enemy ---*/
    const char *p = strstr(log_line, "elem) = ");
    if (p) {
        int val = atoi(p + 8);
        if (val > 0) {
            snprintf(buf, FCT_TEXT_LEN, "-%d", val);
            /* Check if it is a critical hit */
            if (strstr(log_line, "BLOOD!")) {
                fct_spawn(FCT_CRITICAL, 0.0f, -1.0f, buf);
            }
            else {
                fct_spawn(FCT_DAMAGE_OUT, 0.0f, -1.0f, buf);
            }
            return;
        }
    }

    /*--- Damage suffered by the player ---*/
    p = strstr(log_line, "You take ");
    if (p) {
        int val = atoi(p + 9);
        if (val > 0) {
            snprintf(buf, FCT_TEXT_LEN, "-%d", val);
            fct_spawn(FCT_DAMAGE, 0.0f, 0.0f, buf);
            return;
        }
    }

    /*--- Missed shot (enemy misses player) ---*/
    if (strstr(log_line, "missed you")) {
        fct_spawn(FCT_MISS, 0.0f, 0.0f, "MISS");
        return;
    }

    /*--- Missed shot (player misses enemy) ---*/
    if (strstr(log_line, "Missed...")) {
        fct_spawn(FCT_MISS, 0.0f, -1.0f, "MISS");
        return;
    }

    /* --- Level Up --- */
    if (strstr(log_line, "YOU HAVE RISE TO LEVEL")) {
        fct_spawn(FCT_LEVELUP, 0.0f, 0.0f, "LEVEL UP!");
        return;
    }

    /*--- Gold collected ---*/
    p = strstr(log_line, "Collect a stack of ");
    if (!p) {
        p = strstr(log_line, "You have collected ");
    }
    if (p) {
        int val = atoi(p + 19);
        if (val > 0) {
            snprintf(buf, FCT_TEXT_LEN, "+%d gp", val);
            fct_spawn(FCT_GOLD, 0.0f, 0.0f, buf);
            return;
        }
    }

    /* --- Healing --- */
    p = strstr(log_line, "Recover ");
    if (p) {
        int val = atoi(p + 8);
        if (val > 0) {
            snprintf(buf, FCT_TEXT_LEN, "+%d", val);
            fct_spawn(FCT_HEAL, 0.0f, 0.0f, buf);
            return;
        }
    }
}
