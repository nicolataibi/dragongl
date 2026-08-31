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

static FILE* g_log_file = NULL;
static long  g_event_count = 0;

// Utility: timestamp ISO 8601
static void write_timestamp(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", t);
    fprintf(g_log_file, "\"ts\":\"%s\"", buf);
}

void clog_init(const char* filepath) {
    if (g_log_file) {
        fclose(g_log_file);
    }
    g_log_file = fopen(filepath, "w");
    if (!g_log_file) {
        return;
    }
    g_event_count = 0;
    fprintf(g_log_file, "[\n");
    fflush(g_log_file);
}

void clog_close(void) {
    if (!g_log_file) return;
    fprintf(g_log_file, "\n]\n");
    fclose(g_log_file);
    g_log_file = NULL;
}

//Internal helper for JSON entry separator
static void begin_entry(const char* type) {
    if (g_event_count > 0) {
        fprintf(g_log_file, ",\n");
    }
    fprintf(g_log_file, "  {");
    write_timestamp();
    fprintf(g_log_file, ",\"type\":\"%s\"", type);
    g_event_count++;
}

void clog_attack(const char* attacker, const char* target,
                 int roll, int bonus, int ac,
                 bool hit, bool crit, int damage) {
    if (!g_log_file) return;
    begin_entry("attack");
    fprintf(g_log_file,
        ",\"attacker\":\"%s\",\"target\":\"%s\""
        ",\"d20\":%d,\"bonus\":%d,\"ac\":%d"
        ",\"hit\":%s,\"crit\":%s,\"damage\":%d}",
        attacker ? attacker : "?",
        target   ? target   : "?",
        roll, bonus, ac,
        hit  ? "true" : "false",
        crit ? "true" : "false",
        damage);
    fflush(g_log_file);
}

void clog_save(const char* entity, const char* effect,
               int roll, int modifier, int dc, bool passed) {
    if (!g_log_file) return;
    begin_entry("save");
    fprintf(g_log_file,
        ",\"entity\":\"%s\",\"effect\":\"%s\""
        ",\"d20\":%d,\"modifier\":%d,\"dc\":%d,\"passed\":%s}",
        entity ? entity : "?",
        effect ? effect : "?",
        roll, modifier, dc,
        passed ? "true" : "false");
    fflush(g_log_file);
}

void clog_spell(const char* caster, const char* spell_name,
                const char* target, int damage_or_heal, bool saved) {
    if (!g_log_file) return;
    begin_entry("spell");
    fprintf(g_log_file,
        ",\"caster\":\"%s\",\"spell\":\"%s\""
        ",\"target\":\"%s\",\"value\":%d,\"saved\":%s}",
        caster     ? caster     : "?",
        spell_name ? spell_name : "?",
        target     ? target     : "?",
        damage_or_heal,
        saved ? "true" : "false");
    fflush(g_log_file);
}

void clog_death(const char* entity, const char* killer, int floor_id) {
    if (!g_log_file) return;
    begin_entry("death");
    fprintf(g_log_file,
        ",\"entity\":\"%s\",\"killer\":\"%s\",\"floor\":%d}",
        entity ? entity : "?",
        killer ? killer : "?",
        floor_id);
    fflush(g_log_file);
}
