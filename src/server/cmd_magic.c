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
 * cmd_magic.c - Magic commands: spells, cast, pray.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "strnum.h"
#include "server_cmd.h"
#include "server_internal.h"
#include "server_combat.h"
#include "spell_cast.h"
#include "spell_effect.h"
#include "server_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*-------------------------------------------------------
 * spells — lists the spells in your personal grimoire
 * Show only spells in the known_spells bitfield, grouped together
 * per level with the number of slots remaining.
 * -------------------------------------------------------*/
bool cmd_spells(Client *c, const char *cmd, NPC *npcs) {
    (void)cmd;
    (void)npcs;
    int listed = 0;
    /*Group by level: 0 (cantrip) .. MAX_SPELL_LEVEL*/
    for (int lv = 0; lv <= MAX_SPELL_LEVEL; lv++) {
        int lv_count = 0;
        /*First pass: Count how many spells this level has*/
        for (int i = 0; i < spell_database_size; i++) {
            if (spell_database[i].level != lv) {
                continue;
            }
            if (!spell_known_get(c, i)) {
                continue;
            }
            lv_count++;
        }
        if (lv_count == 0) {
            continue;
        }
        /*Level header*/
        if (lv == 0) {
            send_text_to_client(c->sock, "--- Cantrips (Slot: unlimited) ---");
        } else {
            send_text_to_client(c->sock, "--- Level %d (Slot: %d/%d) ---", lv, c->spell_slots[lv],
                                c->spell_slots_max[lv]);
        }
        /*Second pass: List the spells*/
        for (int i = 0; i < spell_database_size; i++) {
            if (spell_database[i].level != lv) {
                continue;
            }
            if (!spell_known_get(c, i)) {
                continue;
            }
            const SpellEffect *e = &spell_database[i].effect;
            send_text_to_client(c->sock, "  cast %-30s  [%s, %s, range %d]", spell_database[i].name,
                                spell_kind_name(e->kind), spell_target_name(e->target), e->range);
            listed++;
        }
    }
    if (listed == 0) {
        send_text_to_client(
            c->sock,
            "[GRIMOIRE] You don't know any magic yet. "
            "Find a magical book and study it with 'study <n>' (in your classroom temple).");
    }
    return true;
}

/*True if a book holding the spell is in the backpack, on the belt or in
 * a hand: a spell is cast from its book.*/
static bool book_holds(const ItemInstance *it, const char *spell) {
    if (it->template_idx < 0) {
        return false;
    }
    const ItemTemplate *t = &item_database[it->template_idx];
    if (t->category != ITEM_BOOK) {
        return false;
    }
    for (int k = 0; k < t->book_spell_count; k++) {
        if (strcasecmp(t->book_spell_names[k], spell) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_book_of(const Client *c, const char *spell) {
    for (int i = 0; i < c->backpack_count; i++) {
        if (book_holds(&c->backpack[i], spell)) {
            return true;
        }
    }
    for (int i = 0; i < MAX_BELT; i++) {
        if (book_holds(&c->belt[i], spell)) {
            return true;
        }
    }
    return book_holds(&c->slot_hand_l, spell) || book_holds(&c->slot_hand_r, spell);
}

/*"cast <book_idx> <spell_idx>": the name of that spell of that book, or
 * the argument unchanged.*/
static const char *spell_by_book_index(const Client *c, const char *arg) {
    int bl[2] = {0};
    if (scan_ints(arg, ' ', 2, bl) != 2) {
        return arg;
    }
    int b_idx = bl[0] - 1; /*1-based in the backpack listing*/
    if (b_idx < 0 || b_idx >= c->backpack_count || c->backpack[b_idx].template_idx < 0) {
        return arg;
    }
    const ItemTemplate *tmpl = &item_database[c->backpack[b_idx].template_idx];
    int local_idx = bl[1] - (tmpl->book_seq * MAX_BOOK_SPELLS);
    if (tmpl->category != ITEM_BOOK || local_idx < 0 || local_idx >= tmpl->book_spell_count) {
        return arg;
    }
    return tmpl->book_spell_names[local_idx];
}

//cast <name> or cast <book_idx> <spell_idx>
bool cmd_cast(Client *c, const char *cmd, NPC *npcs) {
    if (rules_has_condition_t(c->effects, c->effect_count, COND_SILENCED)) {
        send_text_to_client(c->sock, "[SYSTEM] You are silenced and cannot cast spells!");
        return true;
    }
    if (!client_can_act(c)) {
        return true;
    }
    const char *arg = strchr(cmd, ' ');
    if (!arg) {
        return true;
    }
    const char *spell_name = spell_by_book_index(c, arg + 1);
    int sp_idx = -1;
    for (int i = 0; i < spell_database_size && sp_idx < 0; i++) {
        if (strcasecmp(spell_database[i].name, spell_name) == 0) {
            sp_idx = i;
        }
    }
    if (sp_idx == -1) {
        send_text_to_client(c->sock, "[ERROR] Spell '%s' not found.", spell_name);
        return true;
    }
    const SpellTemplate *sp = &spell_database[sp_idx];
    if (!spell_known_get(c, sp_idx)) {
        send_text_to_client(c->sock,
                            "[MAGIC] You don't know '%s'. Find the corresponding book"
                            " and study it at the temple with 'study <n>'.",
                            sp->name);
        return true;
    }
    if (!has_book_of(c, sp->name)) {
        send_text_to_client(
            c->sock,
            "[MAGIC] You have memorized '%s', but the book containing it is not in your inventory! You have to carry it with you to cast it.",
            sp->name);
        return true;
    }
    int slv = sp->level;
    if (slv > 0 && c->spell_slots[slv] <= 0) {
        send_text_to_client(c->sock, "[MAGIC] No level %d slots left.", slv);
        return true;
    }
    /*The slot is spent only when the spell goes off.*/
    if (spell_cast(c, sp, npcs) && slv > 0) {
        c->spell_slots[slv]--;
    }
    return true;
}

bool cmd_pray(Client *c, const char *cmd, NPC *npcs) {
    (void)cmd;
    (void)npcs;
    if (c->xp >= 100) {
        c->xp -= 100;
        int heal = c->max_hp / 4;
        c->hp += heal;
        if (c->hp > c->max_hp) {
            c->hp = c->max_hp;
        }
        send_detailed_state(c); //Sync HP and XP with the HUD client
        send_text_to_client(
            c->sock, "[SYSTEM] You pray hard, sacrificing 100 XP. Restores %d HP (HP: %d/%d).",
            heal, c->hp, c->max_hp);
    } else {
        send_text_to_client(
            c->sock,
            "[SYSTEM] The Gods do not respond. You don't have enough experience (XP) to sacrifice.");
    }
    return true;
}
