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

#include "strnum.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static bool has_digit_ahead(const char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '+' || *s == '-') {
        s++;
    }
    return isdigit((unsigned char)*s) != 0;
}

bool parse_ll(const char *s, long long *out, const char **end) {
    if (!s || !has_digit_ahead(s)) {
        return false;
    }
    char *stop = NULL;
    errno = 0;
    long long v = strtoll(s, &stop, 10);
    if (errno == ERANGE) {
        return false;
    }
    *out = v;
    if (end) {
        *end = stop;
    }
    return true;
}

bool parse_int(const char *s, int *out, const char **end) {
    long long v = 0;
    const char *stop = NULL;
    if (!parse_ll(s, &v, &stop) || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    if (end) {
        *end = stop;
    }
    return true;
}

bool parse_double(const char *s, double *out, const char **end) {
    if (!s) {
        return false;
    }
    char *stop = NULL;
    errno = 0;
    double v = strtod(s, &stop);
    if (stop == s || errno == ERANGE) {
        return false;
    }
    *out = v;
    if (end) {
        *end = stop;
    }
    return true;
}

int str_to_int(const char *s) {
    if (!s || !has_digit_ahead(s)) {
        return 0;
    }
    errno = 0;
    long long v = strtoll(s, NULL, 10);
    if (v > INT_MAX) {
        return INT_MAX;
    }
    if (v < INT_MIN) {
        return INT_MIN;
    }
    return (int)v;
}

int scan_ints(const char *s, char sep, int n, int *out) {
    int got = 0;
    const char *p = s;
    while (p && got < n) {
        if (got > 0) {
            if (sep != ' ') {
                while (isspace((unsigned char)*p)) {
                    p++;
                }
                if (*p != sep) {
                    break;
                }
                p++;
            } else if (!isspace((unsigned char)*p)) {
                break;
            }
        }
        if (!parse_int(p, &out[got], &p)) {
            break;
        }
        got++;
    }
    return got;
}
