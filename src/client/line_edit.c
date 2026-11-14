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
 * line_edit.c - Editing and history of the command line (line_edit.h).
 */

#include "line_edit.h"

#include <string.h>

void line_edit_init(LineEdit *le) {
    memset(le, 0, sizeof(*le));
    le->browsing = -1;
}

static void set_text(LineEdit *le, const char *s) {
    size_t n = strlen(s);
    if (n >= LINE_EDIT_MAX) {
        n = LINE_EDIT_MAX - 1;
    }
    memcpy(le->text, s, n);
    le->text[n] = '\0';
    le->len = (int)n;
    le->cursor = le->len;
}

void line_edit_clear(LineEdit *le) {
    le->text[0] = '\0';
    le->len = 0;
    le->cursor = 0;
    le->browsing = -1;
    le->draft[0] = '\0';
}

bool line_edit_insert(LineEdit *le, unsigned int codepoint) {
    if (codepoint < 32 || codepoint > 126 || le->len >= LINE_EDIT_MAX - 1) {
        return false;
    }
    memmove(le->text + le->cursor + 1, le->text + le->cursor, (size_t)(le->len - le->cursor) + 1);
    le->text[le->cursor] = (char)codepoint;
    le->cursor++;
    le->len++;
    return true;
}

/*Removes the characters from..to (to excluded) and leaves the cursor at
 * from.*/
static void remove_range(LineEdit *le, int from, int to) {
    if (from < 0) {
        from = 0;
    }
    if (to > le->len) {
        to = le->len;
    }
    if (from >= to) {
        return;
    }
    memmove(le->text + from, le->text + to, (size_t)(le->len - to) + 1);
    le->len -= to - from;
    le->cursor = from;
}

void line_edit_backspace(LineEdit *le) {
    remove_range(le, le->cursor - 1, le->cursor);
}

void line_edit_delete(LineEdit *le) {
    remove_range(le, le->cursor, le->cursor + 1);
}

void line_edit_left(LineEdit *le) {
    if (le->cursor > 0) {
        le->cursor--;
    }
}

void line_edit_right(LineEdit *le) {
    if (le->cursor < le->len) {
        le->cursor++;
    }
}

void line_edit_home(LineEdit *le) {
    le->cursor = 0;
}

void line_edit_end(LineEdit *le) {
    le->cursor = le->len;
}

void line_edit_word_left(LineEdit *le) {
    while (le->cursor > 0 && le->text[le->cursor - 1] == ' ') {
        le->cursor--;
    }
    while (le->cursor > 0 && le->text[le->cursor - 1] != ' ') {
        le->cursor--;
    }
}

void line_edit_word_right(LineEdit *le) {
    while (le->cursor < le->len && le->text[le->cursor] != ' ') {
        le->cursor++;
    }
    while (le->cursor < le->len && le->text[le->cursor] == ' ') {
        le->cursor++;
    }
}

void line_edit_kill_before(LineEdit *le) {
    remove_range(le, 0, le->cursor);
}

void line_edit_kill_after(LineEdit *le) {
    remove_range(le, le->cursor, le->len);
}

void line_edit_history_prev(LineEdit *le) {
    if (le->history_count == 0 || le->browsing == 0) {
        return;
    }
    if (le->browsing < 0) {
        memcpy(le->draft, le->text, sizeof(le->draft));
        le->browsing = le->history_count;
    }
    le->browsing--;
    set_text(le, le->history[le->browsing]);
}

void line_edit_history_next(LineEdit *le) {
    if (le->browsing < 0) {
        return;
    }
    le->browsing++;
    if (le->browsing >= le->history_count) {
        le->browsing = -1;
        set_text(le, le->draft);
        return;
    }
    set_text(le, le->history[le->browsing]);
}

static void remember(LineEdit *le, const char *line) {
    if (le->history_count > 0 && strcmp(le->history[le->history_count - 1], line) == 0) {
        return;
    }
    if (le->history_count == LINE_EDIT_HISTORY) {
        memmove(le->history[0], le->history[1], sizeof(le->history[0]) * (LINE_EDIT_HISTORY - 1));
        le->history_count--;
    }
    memcpy(le->history[le->history_count], line, strlen(line) + 1);
    le->history_count++;
}

bool line_edit_submit(LineEdit *le, char *out, int out_len) {
    int from = 0, to = le->len;
    while (from < to && le->text[from] == ' ') {
        from++;
    }
    while (to > from && le->text[to - 1] == ' ') {
        to--;
    }
    int n = to - from;
    if (n >= out_len) {
        n = out_len - 1;
    }
    bool any = n > 0;
    if (any) {
        memcpy(out, le->text + from, (size_t)n);
        out[n] = '\0';
        remember(le, out);
    } else if (out_len > 0) {
        out[0] = '\0';
    }
    line_edit_clear(le);
    return any;
}
