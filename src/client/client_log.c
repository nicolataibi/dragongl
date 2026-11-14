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
 * client_log.c - Message history of the HUD (client_log.h).
 */

#include "client_log.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_lines[CLIENT_LOG_LINES][CLIENT_LOG_TEXT];
static int g_head;  /*slot of the next line*/
static int g_count; /*lines held*/
static unsigned long g_total;

void client_log_add(const char *text) {
    pthread_mutex_lock(&g_lock);
    char *dst = g_lines[g_head];
    size_t n = 0;
    for (; n < CLIENT_LOG_TEXT - 1 && text[n]; n++) {
        dst[n] = text[n];
        if (text[n] < 32 || text[n] > 126) {
            dst[n] = '?';
        }
    }
    dst[n] = '\0';
    g_head = (g_head + 1) % CLIENT_LOG_LINES;
    if (g_count < CLIENT_LOG_LINES) {
        g_count++;
    }
    g_total++;
    pthread_mutex_unlock(&g_lock);
}

int client_log_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

unsigned long client_log_total(void) {
    pthread_mutex_lock(&g_lock);
    unsigned long n = g_total;
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool client_log_get(int back, char *out, size_t cap) {
    bool found = false;
    pthread_mutex_lock(&g_lock);
    if (back >= 0 && back < g_count && cap > 0) {
        int slot = (g_head - 1 - back + 2 * CLIENT_LOG_LINES) % CLIENT_LOG_LINES;
        size_t n = strlen(g_lines[slot]);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, g_lines[slot], n);
        out[n] = '\0';
        found = true;
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

void client_log_clear(void) {
    pthread_mutex_lock(&g_lock);
    g_head = 0;
    g_count = 0;
    g_total = 0;
    pthread_mutex_unlock(&g_lock);
}

int text_wrap_row(const char *s, int cols, int *next) {
    int len = (int)strlen(s);
    if (cols < 1) {
        cols = 1;
    }
    if (len <= cols) {
        *next = len;
        return len;
    }
    int cut = cols;
    while (cut > 0 && s[cut] != ' ') {
        cut--;
    }
    if (cut == 0) {
        cut = cols; /*a word longer than the row: cut it*/
        *next = cut;
        return cut;
    }
    int after = cut;
    while (s[after] == ' ') {
        after++;
    }
    while (cut > 0 && s[cut - 1] == ' ') {
        cut--;
    }
    *next = after;
    return cut;
}

int text_wrap_count(const char *s, int cols) {
    int rows = 0;
    do {
        int next = 0;
        (void)text_wrap_row(s, cols, &next);
        s += next;
        rows++;
    } while (*s);
    return rows;
}
