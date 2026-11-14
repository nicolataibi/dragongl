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
 * line_edit.h - The command line of the window: one line of ASCII text
 * with a cursor and a history of the lines sent. No rendering and no
 * input library here (tests/test_client_ui.c).
 */

#ifndef LINE_EDIT_H
#define LINE_EDIT_H

#include <stdbool.h>

/*Longest line, terminator included: what MSG_TEXT_CMD carries
 * (PROTO_TEXT_LEN).*/
#define LINE_EDIT_MAX     256
#define LINE_EDIT_HISTORY 64

typedef struct {
    char text[LINE_EDIT_MAX];
    int len;
    int cursor; /*0..len: the next character is inserted here*/
    /*Lines sent, oldest first. browsing: index of the entry on the line,
     * or -1 while the player edits a line of his own (kept in draft).*/
    char history[LINE_EDIT_HISTORY][LINE_EDIT_MAX];
    int history_count;
    int browsing;
    char draft[LINE_EDIT_MAX];
} LineEdit;

void line_edit_init(LineEdit *le);
/*Empties the line; the history stays.*/
void line_edit_clear(LineEdit *le);

/*A printable ASCII character at the cursor. False (nothing changes) for
 * any other byte or when the line is full.*/
bool line_edit_insert(LineEdit *le, unsigned int codepoint);
void line_edit_backspace(LineEdit *le);
void line_edit_delete(LineEdit *le);
void line_edit_left(LineEdit *le);
void line_edit_right(LineEdit *le);
void line_edit_home(LineEdit *le);
void line_edit_end(LineEdit *le);
/*To the start of the previous / of the next word.*/
void line_edit_word_left(LineEdit *le);
void line_edit_word_right(LineEdit *le);
/*Deletes from the start of the line to the cursor / from the cursor to
 * the end.*/
void line_edit_kill_before(LineEdit *le);
void line_edit_kill_after(LineEdit *le);

/*Older / newer line of the history. The line being typed comes back
 * after the newest one.*/
void line_edit_history_prev(LineEdit *le);
void line_edit_history_next(LineEdit *le);

/*Ends the line: copies it to out without the blanks around it, remembers
 * it (unless empty or equal to the last one) and empties the line. False
 * when there was nothing but blanks.*/
bool line_edit_submit(LineEdit *le, char *out, int out_len);

#endif /* LINE_EDIT_H */
