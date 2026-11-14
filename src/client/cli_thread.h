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

#ifndef DRAGONGL_CLI_THREAD_H
#define DRAGONGL_CLI_THREAD_H

/* Commands typed in the terminal that started the client. */
void *cli_thread_loop(void *arg);

/* A message of the server, printed on the terminal. On a terminal the
 * prompt "> " is erased first and drawn again after the message; when the
 * output goes to a file or a pipe (client.log) the message is printed as
 * it is, with no prompt and no escape sequence. */
void console_print_line(const char *text);

#endif
