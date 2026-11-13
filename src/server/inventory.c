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
 * inventory.c - Finding and removing the items a character carries.
 */

#include "inventory.h"
#include "strnum.h"
#include <ctype.h>

/*A carried item by number (backpack) or by name (backpack, then belt): its
 * index, with *in_belt set when it is a belt slot; -1 if there is none.*/
int list_index(const char *s) {
    if (!isdigit((unsigned char)s[0])) {
        return -1;
    }
    int n = str_to_int(s);
    return n >= 1 ? n - 1 : -1;
}

int find_carried(const Client *c, const char *query, bool *in_belt) {
    *in_belt = false;
    if (isdigit((unsigned char)query[0])) {
        int idx = list_index(query);
        return idx < c->backpack_count ? idx : -1;
    }
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, query) != NULL) {
            return i;
        }
    }
    for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
            strcasestr(item_database[c->belt[i].template_idx].name, query) != NULL) {
            *in_belt = true;
            return i;
        }
    }
    return -1;
}

/*Takes the item out of the backpack (closing the gap) or off the belt.*/
void remove_carried(Client *c, int idx, bool in_belt) {
    if (in_belt) {
        c->belt[idx].template_idx = -1;
        c->belt[idx].stack_count = 0;
        return;
    }
    for (int i = idx; i < c->backpack_count - 1; i++) {
        c->backpack[i] = c->backpack[i + 1];
    }
    c->backpack_count--;
}
