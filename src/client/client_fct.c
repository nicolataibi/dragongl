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
#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

/*Pre-allocated global array — zero-initialized (all inactive)*/
FloatingText g_fct[FCT_MAX_ENTRIES] = {0};
/*See client_fct.h: guards the pool against the net thread, which spawns
 * concurrently (B1).*/
pthread_mutex_t g_fct_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_fct_mutex);
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
            pthread_mutex_unlock(&g_fct_mutex);
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
    pthread_mutex_unlock(&g_fct_mutex);
}

/* ----------------------------------------------------------------
 * fct_update — Updates position and opacity of every active FCT.
 * ---------------------------------------------------------------- */
void fct_update(float dt) {
    pthread_mutex_lock(&g_fct_mutex);
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
    pthread_mutex_unlock(&g_fct_mutex);
}

/* ----------------------------------------------------------------
 * fct_combat_event — Floating text for a MSG_COMBAT_EVENT.
 *
 * The server states what happened (protocol.h, CombatEventKind); the
 * client no longer guesses it from the wording of MSG_TEXT lines, which
 * broke whenever a message was reworded or translated.
 *
 * Coordinates are relative to the player (the renderer centers the world
 * on the player): (0,0) on the player, (0,-1) just in front for the
 * damage the player deals.
 * ---------------------------------------------------------------- */
void fct_combat_event(int kind, int amount) {
    char buf[FCT_TEXT_LEN];
    switch (kind) {
    case COMBAT_EV_DAMAGE_TAKEN:
        snprintf(buf, sizeof(buf), "-%d", amount);
        fct_spawn(FCT_DAMAGE, 0.0f, 0.0f, buf);
        break;
    case COMBAT_EV_DAMAGE_DEALT:
        snprintf(buf, sizeof(buf), "-%d", amount);
        fct_spawn(FCT_DAMAGE_OUT, 0.0f, -1.0f, buf);
        break;
    case COMBAT_EV_CRITICAL:
        snprintf(buf, sizeof(buf), "-%d", amount);
        fct_spawn(FCT_CRITICAL, 0.0f, -1.0f, buf);
        break;
    case COMBAT_EV_MISS_TAKEN:
        fct_spawn(FCT_MISS, 0.0f, 0.0f, "MISS");
        break;
    case COMBAT_EV_MISS_DEALT:
        fct_spawn(FCT_MISS, 0.0f, -1.0f, "MISS");
        break;
    case COMBAT_EV_HEAL:
        snprintf(buf, sizeof(buf), "+%d", amount);
        fct_spawn(FCT_HEAL, 0.0f, 0.0f, buf);
        break;
    case COMBAT_EV_GOLD:
        snprintf(buf, sizeof(buf), "+%d gp", amount);
        fct_spawn(FCT_GOLD, 0.0f, 0.0f, buf);
        break;
    case COMBAT_EV_LEVEL_UP:
        fct_spawn(FCT_LEVELUP, 0.0f, 0.0f, "LEVEL UP!");
        break;
    default:
        break;
    }
}
