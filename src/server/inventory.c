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
#include "item_effects.h"
#include "strnum.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

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

void item_instance_new(ItemInstance *out, int template_idx) {
    const ItemTemplate *t = &item_database[template_idx];
    memset(out, 0, sizeof(*out));
    out->template_idx = template_idx;
    out->durability = t->max_durability;
    if (t->category == ITEM_LIGHT_SOURCE && strcasestr(t->name, "lantern")) {
        out->durability = 5000;
    }
    out->current_charges = t->max_charges;
    item_roll_variant(out); /*a ring of resistance: the damage type*/
    out->stack_count = 1;
    out->is_identified = true;
    out->quality = QUALITY_NORMAL;
    out->blessing = BLESS_NORMAL;
    out->element = ELEM_NONE;
}

bool item_recharges(const ItemInstance *it) {
    return it->template_idx >= 0 && item_database[it->template_idx].max_charges > 0 &&
           !item_database[it->template_idx].no_recharge;
}

ItemInstance *equip_slot(Client *c, int i) {
    ItemInstance *slots[EQUIP_SLOT_COUNT] = {
        &c->slot_head,   &c->slot_neck,  &c->slot_body,  &c->slot_back,  &c->slot_hand_r,
        &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r, &c->slot_arm_l, &c->slot_feet};
    return (i >= 0 && i < EQUIP_SLOT_COUNT) ? slots[i] : NULL;
}

const char *equip_slot_names(int i) {
    /*"arm" and "arms" take the right arm first.*/
    static const char *const names[EQUIP_SLOT_COUNT] = {"head",
                                                        "neck",
                                                        "body",
                                                        "back",
                                                        "hand_r|right_hand",
                                                        "hand_l|left_hand",
                                                        "hands",
                                                        "arm_r|right_arm|r.a|arm|arms",
                                                        "arm_l|left_arm|l.a|arm|arms",
                                                        "feet"};
    return (i >= 0 && i < EQUIP_SLOT_COUNT) ? names[i] : "";
}

/*True if word is one of the "|"-separated names, case ignored.*/
static bool slot_named(const char *names, const char *word) {
    size_t len = strlen(word);
    for (const char *p = names; p;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == len && strncasecmp(p, word, n) == 0) {
            return true;
        }
        p = bar ? bar + 1 : NULL;
    }
    return false;
}

static bool item_called(const ItemInstance *it, const char *word) {
    return strcasestr(item_database[it->template_idx].name, word) != NULL ||
           (it->is_artifact && strcasestr(it->artifact_name, word) != NULL);
}

ItemInstance *find_equipped(Client *c, const char *word) {
    if (!word || !word[0]) {
        return NULL;
    }
    for (int i = 0; i < EQUIP_SLOT_COUNT; i++) {
        ItemInstance *s = equip_slot(c, i);
        if (s->template_idx != -1 &&
            (slot_named(equip_slot_names(i), word) || item_called(s, word))) {
            return s;
        }
    }
    for (int r = 0; r < 10; r++) {
        ItemInstance *s = &c->slot_rings[r];
        if (s->template_idx != -1 && (slot_named("ring|rings", word) || item_called(s, word))) {
            return s;
        }
    }
    return NULL;
}
