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

#include "combat_log.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

/*--- Flush/rotation policy --------------------------------------------
 * The log used to fflush() after EVERY event (one write() syscall per
 * hit/save/death/spell — hundreds per second in a busy dungeon) and
 * never rotated (a single long session produced a 92 MB file). Now:
 *   - the buffer is flushed at most every CLOG_FLUSH_EVERY events OR
 *     every second, whichever comes first (crash loss is bounded by
 *     ~1 s of events, not the whole session);
 *   - when the current file exceeds CLOG_ROTATE_BYTES it is closed as
 *     VALID JSON (closing bracket written first), renamed to
 *     <name>.1 (overwriting the previous rotation: at most two files
 *     exist) and a fresh one is opened.
 * A new server run still starts a fresh log ("w" on init). The file is
 * git-ignored. ----------------------------------------------------------------*/
#define CLOG_ROTATE_BYTES (10LL * 1024 * 1024) /*10 MB per file*/
#define CLOG_FLUSH_EVERY  16                   /*events per forced flush*/
#define CLOG_PATH_MAX     256

static FILE*     g_log_file = NULL;
static char      g_log_path[CLOG_PATH_MAX] = {0};
static long      g_file_entries = 0;   /*entries in the CURRENT file*/
static long      g_unflushed = 0;      /*entries since last fflush*/
static long long g_last_flush_ms = 0;

static long long clog_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*Write a JSON string VALUE (quotes included) with proper escaping (M3):
 * monster names come from bestiary.json, which users extend by design —
 * a name containing " or \ interpolated raw used to corrupt the whole
 * log. Control characters (< 0x20) become \u00XX, so the file is
 * always valid JSON/UTF-8 no matter what the data sets contain.*/
static void write_json_string(const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "?");
    fputc('"', g_log_file);
    while (*p) {
        switch (*p) {
        case '"':  fputs("\\\"", g_log_file); break;
        case '\\': fputs("\\\\", g_log_file); break;
        case '\b': fputs("\\b", g_log_file);  break;
        case '\f': fputs("\\f", g_log_file);  break;
        case '\n': fputs("\\n", g_log_file);  break;
        case '\r': fputs("\\r", g_log_file);  break;
        case '\t': fputs("\\t", g_log_file);  break;
        default:
            if (*p < 0x20)
                fprintf(g_log_file, "\\u%04x", (unsigned)*p);
            else
                fputc(*p, g_log_file);
        }
        p++;
    }
    fputc('"', g_log_file);
}

/*Flush if the policy says so, then rotate if the file grew past the
 * limit. Called at the end of every logged entry.*/
static void clog_maybe_flush_and_rotate(void) {
    if (!g_log_file)
        return;
    long long now_ms = clog_now_ms();
    if (g_unflushed >= CLOG_FLUSH_EVERY || now_ms - g_last_flush_ms >= 1000) {
        if (fflush(g_log_file) == 0)
            g_unflushed = 0;
        g_last_flush_ms = now_ms;
    }
    if (ftell(g_log_file) >= CLOG_ROTATE_BYTES) {
        /*Close the current file as valid JSON, then move it aside.*/
        fprintf(g_log_file, "\n]\n");
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
        char old[CLOG_PATH_MAX + 8];
        snprintf(old, sizeof(old), "%s.1", g_log_path);
        rename(g_log_path, old); /*overwrites the previous rotation*/
        g_log_file = fopen(g_log_path, "w");
        if (g_log_file) {
            g_file_entries = 0;
            fprintf(g_log_file, "[\n");
            fflush(g_log_file);
        }
    }
}

/*Utility: timestamp ISO 8601 (escaped through write_json_string — it is
 * a constant, but the path is the same for every field).*/
static void write_timestamp(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", t);
    fputs("\"ts\":", g_log_file);
    write_json_string(buf);
}

void clog_init(const char* filepath) {
    if (g_log_file) {
        fclose(g_log_file);
    }
    g_log_file = fopen(filepath, "w");
    if (!g_log_file) {
        g_log_path[0] = '\0';
        return;
    }
    snprintf(g_log_path, sizeof(g_log_path), "%s", filepath);
    g_file_entries = 0;
    g_unflushed = 0;
    g_last_flush_ms = clog_now_ms();
    fprintf(g_log_file, "[\n");
    fflush(g_log_file);
}

void clog_close(void) {
    if (!g_log_file) return;
    fprintf(g_log_file, "\n]\n");
    fflush(g_log_file);
    fclose(g_log_file);
    g_log_file = NULL;
}

/*Internal helper for JSON entry separator (per CURRENT file: the first
 * entry of a rotated file must not start with a comma).*/
static void begin_entry(const char* type) {
    if (g_file_entries > 0) {
        fprintf(g_log_file, ",\n");
    }
    fprintf(g_log_file, "  {");
    write_timestamp();
    fputs(",\"type\":", g_log_file);
    write_json_string(type);
    g_file_entries++;
    g_unflushed++;
}

void clog_attack(const char* attacker, const char* target,
                 int roll, int bonus, int ac,
                 bool hit, bool crit, int damage) {
    if (!g_log_file) return;
    begin_entry("attack");
    fputs(",\"attacker\":", g_log_file);
    write_json_string(attacker);
    fputs(",\"target\":", g_log_file);
    write_json_string(target);
    fprintf(g_log_file,
        ",\"d20\":%d,\"bonus\":%d,\"ac\":%d"
        ",\"hit\":%s,\"crit\":%s,\"damage\":%d}",
        roll, bonus, ac,
        hit  ? "true" : "false",
        crit ? "true" : "false",
        damage);
    clog_maybe_flush_and_rotate();
}

void clog_save(const char* entity, const char* effect,
               int roll, int modifier, int dc, bool passed) {
    if (!g_log_file) return;
    begin_entry("save");
    fputs(",\"entity\":", g_log_file);
    write_json_string(entity);
    fputs(",\"effect\":", g_log_file);
    write_json_string(effect);
    fprintf(g_log_file,
        ",\"d20\":%d,\"modifier\":%d,\"dc\":%d,\"passed\":%s}",
        roll, modifier, dc,
        passed ? "true" : "false");
    clog_maybe_flush_and_rotate();
}

void clog_spell(const char* caster, const char* spell_name,
                const char* target, int damage_or_heal, bool saved) {
    if (!g_log_file) return;
    begin_entry("spell");
    fputs(",\"caster\":", g_log_file);
    write_json_string(caster);
    fputs(",\"spell\":", g_log_file);
    write_json_string(spell_name);
    fputs(",\"target\":", g_log_file);
    write_json_string(target);
    fprintf(g_log_file,
        ",\"value\":%d,\"saved\":%s}",
        damage_or_heal,
        saved ? "true" : "false");
    clog_maybe_flush_and_rotate();
}

void clog_death(const char* entity, const char* killer, int floor_id) {
    if (!g_log_file) return;
    begin_entry("death");
    fputs(",\"entity\":", g_log_file);
    write_json_string(entity);
    fputs(",\"killer\":", g_log_file);
    write_json_string(killer);
    fprintf(g_log_file,
        ",\"floor\":%d}",
        floor_id);
    clog_maybe_flush_and_rotate();
}
