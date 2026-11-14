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
 * client_log.h - The messages of the server, kept for the message panel
 * of the HUD: the last CLIENT_LOG_LINES lines, written by the network
 * thread and read by the render thread (its own lock inside).
 */

#ifndef CLIENT_LOG_H
#define CLIENT_LOG_H

#include <stdbool.h>
#include <stddef.h>

#define CLIENT_LOG_LINES 256
#define CLIENT_LOG_TEXT  256

/*Adds a line (cut to CLIENT_LOG_TEXT - 1 bytes; characters outside
 * printable ASCII become '?': the font has nothing else). An empty text is
 * kept as an empty line.*/
void client_log_add(const char *text);
/*Lines held now, and lines added since the start.*/
int client_log_count(void);
unsigned long client_log_total(void);
/*The line "back" places before the newest one (0: the newest). False
 * past the oldest.*/
bool client_log_get(int back, char *out, size_t cap);
void client_log_clear(void);

/*Where to break a text for a column of cols characters: the number of
 * characters of its first row (up to the last blank that fits, or cols
 * when a word is longer than the row). *next: offset of the row after it
 * (the blanks at the break are skipped). 0 for an empty text.*/
int text_wrap_row(const char *s, int cols, int *next);
/*Rows a text takes in a column of cols characters (at least 1).*/
int text_wrap_count(const char *s, int cols);

#endif /* CLIENT_LOG_H */
