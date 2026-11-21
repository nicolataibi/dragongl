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
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_lines[CLIENT_LOG_LINES][CLIENT_LOG_TEXT];
static int g_head;  /*slot of the next line*/
static int g_count; /*lines held*/
static unsigned long g_total;

static char g_epilogue[CLIENT_EPILOGUE_LINES][CLIENT_LOG_TEXT];
static int g_epilogue_count;
static bool g_epilogue_shown;

/*text into dst, cut to CLIENT_LOG_TEXT - 1 bytes, '?' for what the font
 * cannot draw.*/
static void copy_printable(char *dst, const char *text) {
    size_t n = 0;
    for (; n < CLIENT_LOG_TEXT - 1 && text[n]; n++) {
        dst[n] = text[n];
        if (text[n] < 32 || text[n] > 126) {
            dst[n] = '?';
        }
    }
    dst[n] = '\0';
}

bool client_epilogue_take(const char *text) {
    static const char tag[] = "[EPILOGUE] ";
    if (strncmp(text, tag, sizeof(tag) - 1) != 0) {
        return false;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_epilogue_shown) {
        g_epilogue_count = 0; /*a new epilogue, not the end of an old one*/
    }
    if (g_epilogue_count < CLIENT_EPILOGUE_LINES) {
        copy_printable(g_epilogue[g_epilogue_count++], text + sizeof(tag) - 1);
    }
    g_epilogue_shown = true;
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool client_epilogue_visible(void) {
    pthread_mutex_lock(&g_lock);
    bool shown = g_epilogue_shown;
    pthread_mutex_unlock(&g_lock);
    return shown;
}

void client_epilogue_close(void) {
    pthread_mutex_lock(&g_lock);
    g_epilogue_shown = false;
    pthread_mutex_unlock(&g_lock);
}

int client_epilogue_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_epilogue_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool client_epilogue_get(int i, char *out, size_t cap) {
    pthread_mutex_lock(&g_lock);
    bool ok = i >= 0 && i < g_epilogue_count && cap > 0;
    if (ok) {
        snprintf(out, cap, "%s", g_epilogue[i]);
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void client_log_add(const char *text) {
    pthread_mutex_lock(&g_lock);
    copy_printable(g_lines[g_head], text);
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
