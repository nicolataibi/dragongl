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
 * server_log.c - Server log line and the monotonic clock.
 */

#include "server_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/*CLOCK_MONOTONIC: an adjustment of the wall clock (NTP, an operator) must
 * neither stall the game nor make it run the rounds it "missed".*/
long long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*Called from the game loop and from the writer and login threads:
 * localtime_r instead of the shared static of localtime(), and the stream
 * lock held across the three writes so lines never interleave.*/
void server_log(const char *cat, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tm_now;
    char b[20] = "--:--:--";
    if (localtime_r(&now, &tm_now)) {
        strftime(b, sizeof(b), "%H:%M:%S", &tm_now);
    }
    flockfile(stdout);
    printf("[%s] [%-8s] ", b, cat);
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    fflush(stdout);
    funlockfile(stdout);
}
