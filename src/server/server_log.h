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
 * server_log.h - The server log line and the two clocks.
 */

#ifndef SERVER_LOG_H
#define SERVER_LOG_H

/*One line "[HH:MM:SS] [CAT] text" on stdout; safe from every thread.*/
void server_log(const char *cat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/*Monotonic clock, milliseconds.*/
long long get_time_ms(void);

#endif /* SERVER_LOG_H */
