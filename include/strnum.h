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
 * strnum.h - Numbers parsed from text with their errors reported.
 *
 * atoi(), atof() and sscanf("%d") have undefined behaviour on overflow and
 * cannot tell "0" from "not a number". These helpers use strtol/strtod,
 * reject out-of-range values and say what they consumed.
 */
#ifndef STRNUM_H
#define STRNUM_H

#include <stdbool.h>

/* An int at the start of s (leading spaces and a sign allowed, like %d).
 * False if there are no digits or the value does not fit an int; *end
 * (if not NULL) points after the digits on success. */
bool parse_int(const char *s, int *out, const char **end);
bool parse_ll(const char *s, long long *out, const char **end);
bool parse_double(const char *s, double *out, const char **end);

/* atoi() without undefined behaviour: 0 when s holds no number, the
 * value clamped to the int range otherwise. */
int str_to_int(const char *s);

/* Reads up to n ints separated by sep (' ' = any run of spaces, otherwise
 * that one character, with spaces allowed around it), like sscanf with n
 * %d conversions. Returns how many were read before the first failure. */
int scan_ints(const char *s, char sep, int n, int *out);

#endif /* STRNUM_H */
