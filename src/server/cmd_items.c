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
 * cmd_items.c - Inventory commands: inventory, wear, remove, belt, unbelt, lore, fill,
 * use (and its quaff/read/eat/zap and ritual aliases), get, drop, throw,
 * pickup, retrieve.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "inventory.h"
#include "strnum.h"
#include "server_cmd.h"
#include "server_internal.h"
#include "item_effects.h"
#include "player.h"
#include "server_combat.h"
#include "spell_cast.h"
#include <math.h>
#include <ctype.h>

/*The index of the spell called name (case ignored), or -1.*/
static int spell_index_named(const char *name) {
    for (int k = 0; k < spell_database_size; k++) {
        if (strcasecmp(spell_database[k].name, name) == 0) {
            return k;
        }
    }
    return -1;
}

static void consume_one(Client *c, int idx, bool in_belt);

/*retrieve: next to his own ghost, the player lays it to rest (his soul)
 * and takes back what his tombstone on this floor holds (his equipment),
 * wherever the tombstone lies. What does not fit stays in the tombstone.*/
bool cmd_retrieve(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    int ghost_npc_idx = -1;

    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (ghost_is_of(&game->npcs[i], c) && abs(game->npcs[i].x - c->x) <= 1 &&
            abs(game->npcs[i].y - c->y) <= 1) {
            ghost_npc_idx = i;
            break;
        }
    }
    if (ghost_npc_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] No ghost of yours next to you.");
        return true;
    }
    ghost_retrieve(game, c, &game->npcs[ghost_npc_idx]);
    return true;
}

void ghost_retrieve(Game *game, Client *c, NPC *ghost) {
    send_text_to_client(c->sock, "[SYSTEM] You have recovered your soul: your ghost finds peace.");
    npc_set_active(game, ghost, false);
    ghost->respawn_timer = 0;
    Floor *fl = &game->world->floors[c->floor_id];
    if (fl->entity_grid[ghost->y][ghost->x] == ghost->entity_id) {
        fl->entity_grid[ghost->y][ghost->x] = 0;
    }
    if (!tombstone_recover_own(game, c)) {
        send_text_to_client(c->sock, "[SYSTEM] No tombstone of yours lies on this floor.");
    }
    send_detailed_state(game, c);
    save_player_data(c);
}

bool ghost_is_of(const NPC *n, const Client *c) {
    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "Ghost of %s", c->username);
    return n->is_ghost && strcmp(n->custom_name, expected_name) == 0;
}

bool cmd_inventory(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    float w = get_current_weight(c);
    send_text_to_client(c->sock, "================ INVENTORY (%.1f/%.1f kg) ================", w,
                        (float)(c->str * 5));

    ItemInstance *sl[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                          &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                          &c->slot_arm_l,  &c->slot_feet};
    const char *ns[] = {"HEAD",   "NECK",  "BODY",  "BACK",  "R.HAND",
                        "L.HAND", "HANDS", "R.ARM", "L.ARM", "FEET"};

    send_text_to_client(c->sock, "--- EQUIPPED ---");
    for (int i = 0; i < 10; i++) {
        if (sl[i]->template_idx != -1) {
            char buf[128];
            get_full_item_name(sl[i], buf, sizeof(buf));
            send_text_to_client(c->sock, " [%-7s] %s (Dur:%d/%d)", ns[i], buf, sl[i]->durability,
                                item_database[sl[i]->template_idx].max_durability);
        }
    }

    // Ring display
    bool has_rings = false;
    for (int i = 0; i < 10; i++) {
        if (c->slot_rings[i].template_idx != -1) {
            if (!has_rings) {
                send_text_to_client(c->sock, "--- RINGS ---");
                has_rings = true;
            }
            char buf[128];
            get_full_item_name(&c->slot_rings[i], buf, sizeof(buf));
            send_text_to_client(c->sock, " [RING %d] %s", i + 1, buf);
        }
    }

    send_text_to_client(c->sock, "--- ON BELT ---");
    for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1) {
            char buf[128];
            get_full_item_name(&c->belt[i], buf, sizeof(buf));
            send_text_to_client(c->sock, " [SLOT %d] %s (x%d)", i + 1, buf, c->belt[i].stack_count);
        }
    }

    send_text_to_client(c->sock, "--- IN BACKPACK ---");
    if (c->backpack_count == 0) {
        send_text_to_client(c->sock, "  (empty backpack)");
    } else {
        for (int i = 0; i < c->backpack_count; i++) {
            char buf[128];
            get_full_item_name(&c->backpack[i], buf, sizeof(buf));
            send_text_to_client(c->sock, " [%d] %s (x%d)", i + 1, buf, c->backpack[i].stack_count);
        }
    }
    send_text_to_client(c->sock, "==========================================================");
    return true;
}

bool cmd_lore(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = strchr(cmd, ' ');
    if (input) {
        input++;
    } else {
        send_text_to_client(
            c->sock, "[ERROR] Specify the object to inspect (e.g. 'lore 1' or 'lore book').");
        return true;
    }
    int found_idx = -1;
    if (isdigit(input[0])) {
        int requested_idx = list_index(input);
        if (requested_idx >= 0 && requested_idx < c->backpack_count) {
            found_idx = requested_idx;
        }
    } else {
        for (int i = 0; i < c->backpack_count; i++) {
            if (c->backpack[i].template_idx != -1 &&
                strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
                found_idx = i;
                break;
            }
        }
    }

    if (found_idx == -1) {
        send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
        return true;
    }

    if (!c->backpack[found_idx].is_identified) {
        send_text_to_client(c->sock, "[ERROR] You must first identify this object.");
        return true;
    }

    ItemTemplate *tmpl = &item_database[c->backpack[found_idx].template_idx];
    send_text_to_client(c->sock, "[LORE] %s: %s", tmpl->name, tmpl->description);

    if (tmpl->category == ITEM_BOOK) {
        send_text_to_client(c->sock, "--- Spell Index ---");
        if (tmpl->book_spell_count > 0) {
            for (int local_idx = 0; local_idx < tmpl->book_spell_count; local_idx++) {
                int global_idx = tmpl->book_seq * MAX_BOOK_SPELLS + local_idx;
                int si = spell_index_named(tmpl->book_spell_names[local_idx]);
                if (si < 0) {
                    send_text_to_client(c->sock, "  [#%2d] %-25s (unknown spell)", global_idx,
                                        tmpl->book_spell_names[local_idx]);
                    continue;
                }
                send_text_to_client(c->sock, "  [#%2d] %-25s (Lv%d, learned at level %d)",
                                    global_idx, tmpl->book_spell_names[local_idx],
                                    spell_database[si].level,
                                    spell_required_level(&spell_database[si]));
            }
        } else {
            send_text_to_client(c->sock, "(No readable spells listed here)");
        }
        send_text_to_client(c->sock, "--------------------------------");
    }
    return true;
}

/*The lantern in a hand whose name contains input, or NULL.*/
static ItemInstance *lantern_in_hand(Client *c, const char *input) {
    ItemInstance *hands[2] = {&c->slot_hand_r, &c->slot_hand_l};
    for (int h = 0; h < 2; h++) {
        if (hands[h]->template_idx != -1 &&
            strcasestr(item_database[hands[h]->template_idx].name, input) != NULL) {
            return hands[h];
        }
    }
    return NULL;
}

/*fill <n|lantern>: refills a lantern (in a hand, on the belt or in the
 * backpack) with a flask of oil from the backpack.*/
bool cmd_fill(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = cmd + 5;
    int lantern_idx = -1;
    bool in_belt = false;
    ItemInstance *held = isdigit((unsigned char)input[0]) ? NULL : lantern_in_hand(c, input);
    if (held) {
        lantern_idx = 0; /*not an index: target is the hand slot*/
    } else if (isdigit(input[0])) {
        int requested_idx = list_index(input);
        if (requested_idx >= 0 && requested_idx < c->backpack_count) {
            lantern_idx = requested_idx;
        }
    } else {
        for (int i = 0; i < c->backpack_count; i++) {
            if (c->backpack[i].template_idx != -1 &&
                strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
                lantern_idx = i;
                break;
            }
        }
        if (lantern_idx == -1) {
            for (int i = 0; i < MAX_BELT; i++) {
                if (c->belt[i].template_idx != -1 &&
                    strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
                    lantern_idx = i;
                    in_belt = true;
                    break;
                }
            }
        }
    }

    if (lantern_idx == -1) {
        send_text_to_client(
            c->sock, "[ERROR] Item to fill not found (search by name or number in backpack).");
        return true;
    }

    ItemInstance *target = held;
    if (!target) {
        target = in_belt ? &c->belt[lantern_idx] : &c->backpack[lantern_idx];
    }
    ItemTemplate *ltmpl = &item_database[target->template_idx];

    if (ltmpl->category != ITEM_LIGHT_SOURCE) {
        send_text_to_client(c->sock, "[ERROR] '%s' is not a rechargeable light source.",
                            ltmpl->name);
        return true;
    }

    if (strcasestr(ltmpl->name, "torch") || strcasestr(ltmpl->name, "Torcia")) {
        send_text_to_client(c->sock,
                            "[ERROR] The torches just wear out, they cannot be refilled with oil.");
        return true;
    }

    int fuel_idx = -1;
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            item_database[c->backpack[i].template_idx].category == ITEM_FUEL) {
            fuel_idx = i;
            break;
        }
    }

    if (fuel_idx == -1) {
        send_text_to_client(c->sock,
                            "[ERROR] You don't have any oil or fuel in your backpack to refill.");
        return true;
    }

    ItemInstance *fuel = &c->backpack[fuel_idx];
    ItemTemplate *ftmpl = &item_database[fuel->template_idx];

    if (target->durability >= ltmpl->max_durability) {
        send_text_to_client(c->sock, "[OBJECT] %s is already full of fuel.", ltmpl->name);
        return true;
    }

    target->durability = ltmpl->max_durability;
    send_text_to_client(c->sock, "[OBJECT] You filled %s using a dose of %s.", ltmpl->name,
                        ftmpl->name);

    consume_one(c, fuel_idx, false);
    send_detailed_state(game, c);
    return true;
}

/*The temple of the character's class, or NULL.*/
static const TempleInfo *class_temple(const Client *c) {
    for (int ti = 0; ti < TEMPLE_COUNT; ti++) {
        if (TEMPLES[ti].cls == (int)c->class_id) {
            return &TEMPLES[ti];
        }
    }
    return NULL;
}

typedef struct {
    int learned, locked, already;
} StudyTally;

/*LORE.md, Spellcaster Grimoires: level x 2 + level / 2 (cantrips 1): L1 at
 * 2, L2 at 5, L3 at 7 ... L8 at 20; L9 at 20 too, the level cap (the
 * formula would ask 22, a level no character reaches).*/
int spell_required_level(const SpellTemplate *sp) {
    if (sp->level <= 0) {
        return 1;
    }
    int level = sp->level * 2 + sp->level / 2;
    return level > 20 ? 20 : level;
}

/*One spell of the book: learned if the class may cast it and the
 * character has its required level (spell_required_level).*/
static void study_spell(Client *c, const char *spell_name, StudyTally *tally) {
    int si = spell_index_named(spell_name);
    if (si < 0 || !(spell_database[si].class_mask & (1u << (int)c->class_id))) {
        return;
    }
    const SpellTemplate *sp = &spell_database[si];
    int required_level = spell_required_level(sp);
    if (c->level < required_level) {
        send_text_to_client(c->sock, "  [-] %-30s  (requires level %d - LOCKED)", sp->name,
                            required_level);
        tally->locked++;
    } else if (spell_known_get(c, si)) {
        send_text_to_client(c->sock, "  [Lv%d] %-30s  (already in grimoire)", sp->level, sp->name);
        tally->already++;
    } else {
        spell_known_set(c, si);
        send_text_to_client(c->sock, "  [Lv%d] %-30s  *** LEARNED ***", sp->level, sp->name);
        tally->learned++;
    }
}

static void study_summary(Client *c, const StudyTally *t) {
    if (t->learned == 0 && t->already == 0 && t->locked == 0) {
        send_text_to_client(c->sock, "[RITUAL] No compatible spells found for your class.");
        return;
    }
    if (t->learned > 0) {
        send_text_to_client(c->sock,
                            "[RITUAL] You have learned %d new spell(s)! Use 'spells' to "
                            "see your updated library.",
                            t->learned);
    }
    if (t->locked > 0) {
        send_text_to_client(c->sock,
                            "[RITUAL] %d spell(s) are still blocked by your level. "
                            "Keep the book and return when you are more powerful.",
                            t->locked);
    }
    if (t->learned == 0 && t->locked == 0) {
        send_text_to_client(c->sock,
                            "[RITUAL] You already know all the accessible magic in this book.");
    }
}

/*Learns the spells of a book. The player must use the ritual word of his
 * class (study, pray, awaken, ...) inside the temple of his class on floor
 * 0, and learns the spells his level allows. The book is never consumed: a
 * spell is cast from its book, so it must be kept.*/
static void learn_from_book(Client *c, int number, ItemTemplate *tmpl, const char *ritual_word) {
    if (!ritual_word) {
        send_text_to_client(
            c->sock,
            "[ERROR] Use 'study' (or your class ritual) to learn from this book, or use 'lore' to browse it.");
        return;
    }
    if (!(tmpl->book_class_mask & (1u << (int)c->class_id))) {
        send_text_to_client(c->sock,
                            "[BOOK] '%s' is written in a language that your class "
                            "(%s) does not include.",
                            tmpl->name, CLASSES[c->class_id].name);
        return;
    }
    const TempleInfo *temple = class_temple(c);
    if (!temple) {
        send_text_to_client(c->sock,
                            "[BOOK] Class %s does not have a learning temple: "
                            "you cannot activate this book.",
                            CLASSES[c->class_id].name);
        return;
    }
    if (strcmp(ritual_word, temple->ritual) != 0) {
        send_text_to_client(c->sock, "[RITUAL] A %s learns with '%s %d', not with '%s'.",
                            CLASSES[c->class_id].name, temple->ritual, number, ritual_word);
        return;
    }
    bool in_temple = (c->floor_id == 0 && c->x >= temple->x0 && c->x <= temple->x1 &&
                      c->y >= temple->y0 && c->y <= temple->y1);
    if (!in_temple) {
        send_text_to_client(c->sock,
                            "[RITUAL] To activate '%s' you must go to floor 0, in %s, "
                            "and use the command '%s %d' while you are physically inside.",
                            tmpl->name, temple->name, temple->ritual, number);
        return;
    }
    send_text_to_client(c->sock, "[RITUAL] %s... '%s' (you are in %s, level %d):", temple->ritual,
                        tmpl->name, temple->name, c->level);
    StudyTally tally = {0, 0, 0};
    for (int i = 0; i < tmpl->book_spell_count; i++) {
        study_spell(c, tmpl->book_spell_names[i], &tally);
    }
    study_summary(c, &tally);
    c->needs_study = false;
}

/*The ritual word the command starts with (study, pray, awaken, ...), or
 * NULL for any other word.*/
static const char *ritual_word_of(const char *cmd) {
    static const char *const k_rituals[] = {"study",  "pray",      "awaken",    "invoke",
                                            "intone", "celebrate", "pronounce", "practice",
                                            "drill",  "rage",      "sneak",     "meditate"};
    size_t word = strcspn(cmd, " ");
    for (size_t i = 0; i < sizeof(k_rituals) / sizeof(k_rituals[0]); i++) {
        if (strlen(k_rituals[i]) == word && strncmp(cmd, k_rituals[i], word) == 0) {
            return k_rituals[i];
        }
    }
    return NULL;
}

/*One unit of the carried item is used up; the last one leaves the
 * backpack (closing the gap) or the belt.*/
static void consume_one(Client *c, int idx, bool in_belt) {
    ItemInstance *it = in_belt ? &c->belt[idx] : &c->backpack[idx];
    if (--it->stack_count <= 0) {
        remove_carried(c, idx, in_belt);
    }
}

/*A burst of light around the player, in a colour derived from the item's
 * name (the same item always has the same colour).*/
static void item_use_vfx(Game *game, const Client *c, const char *name) {
    unsigned int hash = 5381;
    for (int i = 0; name[i] != '\0'; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)name[i];
    }
    float col_r = 0.2f + (float)(hash % 80) / 100.0f;
    float col_g = 0.2f + (float)((hash >> 8) % 80) / 100.0f;
    float col_b = 0.2f + (float)((hash >> 16) % 80) / 100.0f;
    float m_col = fmaxf(col_r, fmaxf(col_g, col_b));
    if (m_col < 1.0f) {
        col_r /= m_col;
        col_g /= m_col;
        col_b /= m_col;
    }
    broadcast_spell_vfx(game, c->x, c->y, c->x, c->y, 2, col_r, col_g, col_b, c->floor_id);
}

/*Oil refills the first lantern (light radius > 6) in a hand or on the
 * belt; false after the message when there is none.*/
static bool use_fuel(Client *c, const ItemTemplate *fuel) {
    ItemInstance *ls[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                          &c->belt[1],     &c->belt[2],     &c->belt[3]};
    for (int j = 0; j < 6; j++) {
        if (ls[j]->template_idx == -1) {
            continue;
        }
        const ItemTemplate *lit = &item_database[ls[j]->template_idx];
        if (lit->category == ITEM_LIGHT_SOURCE && lit->light_radius > 6) {
            ls[j]->durability = lit->max_durability;
            send_text_to_client(c->sock, "[SYSTEM] You have replenished your %s with %s.",
                                lit->name, fuel->name);
            return true;
        }
    }
    send_text_to_client(c->sock,
                        "[ERROR] You don't have a lantern to refill in hand or on the belt.");
    return false;
}

/*Food: a ration heals a little and ends hunger.*/
static void use_healing(Game *game, Client *c, const ItemTemplate *tmpl) {
    int healed = tmpl->heal_amount;
    c->hp = c->hp + healed > c->max_hp ? c->max_hp : c->hp + healed;
    if (item_is_food(tmpl)) {
        c->hunger_level = 0;
        send_text_to_client(c->sock, "[ITEM] You ate %s. Hunger fully satisfied! (+%d HP).",
                            tmpl->name, healed);
        return;
    }
    item_use_vfx(game, c, tmpl->name);
    send_text_to_client(c->sock, "[ITEM] You used %s and recovered %d HP (HP: %d/%d).", tmpl->name,
                        healed, c->hp, c->max_hp);
}

/*An item with a body slot (a helm, a ring, a cloak, a staff in hand) casts
 * its spell only while worn or held; a wand, a gem or a figurine works
 * from the backpack.*/
static bool needs_wearing(const ItemTemplate *t) {
    switch (t->category) {
    case ITEM_MISC:
    case ITEM_CONSUMABLE:
    case ITEM_FUEL:
    case ITEM_BOOK:
    case ITEM_AMMO:
    case ITEM_LIGHT_SOURCE:
        return false;
    default:
        return true;
    }
}

static const char *magic_verb(const ItemTemplate *t) {
    if (t->is_scroll) {
        return "read";
    }
    if (t->is_potion) {
        return "drink";
    }
    return t->max_charges > 0 && !needs_wearing(t) ? "zap" : "invoke";
}

/*Whether the item can cast now: not silenced for a scroll, worn for an
 * item that must be, charges enough for its cost.*/
static bool magic_item_ready(Client *c, const ItemInstance *it, const ItemTemplate *t,
                             bool equipped) {
    bool charged = t->max_charges > 0;
    if (t->is_scroll && rules_has_condition_t(c->effects, c->effect_count, COND_SILENCED)) {
        send_text_to_client(c->sock, "[ITEM] You are silenced: you cannot read %s aloud.", t->name);
        return false;
    }
    if ((charged || t->at_will) && needs_wearing(t) && !equipped) {
        send_text_to_client(c->sock, "[ITEM] Wear or hold %s to use its magic.", t->name);
        return false;
    }
    if (charged && it->current_charges < t->charge_cost) {
        send_text_to_client(c->sock,
                            t->no_recharge ? "[ITEM] %s is spent: its magic is gone."
                                           : "[ITEM] %s has no charges left: rest in town to "
                                             "recharge it.",
                            t->name);
        return false;
    }
    return true;
}

/*An item that casts its spell as the user would (no slot, no book, no
 * spell known): a scroll is read and a potion drunk (used up), a wand or a
 * charged item spends its charges, an at-will item nothing. Nothing is
 * spent when the spell does not go off. idx < 0: the item is equipped.*/
static void use_magic_item(Game *game, Client *c, ItemInstance *it, int idx, bool in_belt) {
    const ItemTemplate *t = &item_database[it->template_idx];
    const SpellTemplate *sp = &spell_database[t->cast_spell_idx];
    bool equipped = idx < 0;
    if (!client_can_act(c) || !magic_item_ready(c, it, t, equipped)) {
        return;
    }
    send_text_to_client(c->sock, "[ITEM] You %s %s: %s!", magic_verb(t), t->name, sp->name);
    if (!spell_cast(c, sp, game)) {
        return;
    }
    if (!equipped) {
        it = in_belt ? &c->belt[idx] : &c->backpack[idx];
    }
    if (t->max_charges > 0) {
        it->current_charges -= t->charge_cost;
        send_text_to_client(c->sock, "[ITEM] %s: %d of %d charges left.", t->name,
                            it->current_charges, t->max_charges);
    } else if (!t->at_will) {
        consume_one(c, idx, in_belt);
    }
    send_detailed_state(game, c);
}

/*A consumable without a spell: its "use" effects, food, lamp oil; a
 * potion whose magic has no hold in the dungeon (water breathing, flying)
 * is kept. Used up only when it worked.*/
static void use_consumable(Game *game, Client *c, int idx, bool in_belt, const ItemTemplate *t) {
    if (t->use.any) {
        item_use_vfx(game, c, t->name);
        if (!item_use_apply(game, c, t)) {
            return;
        }
        if (c->hp <= 0) {
            consume_one(c, idx, in_belt);
            player_dies(game, c, "was killed by a potion");
            return;
        }
    } else if (t->category == ITEM_FUEL) {
        if (!use_fuel(c, t)) {
            return;
        }
    } else if (t->heal_amount > 0) {
        use_healing(game, c, t);
    } else if (t->is_potion) {
        send_text_to_client(
            c->sock, "[ITEM] The magic of %s has no hold in the dungeon: you keep it.", t->name);
        return;
    } else {
        send_text_to_client(c->sock, "[ERROR] %s is not consumable or usable as fuel.", t->name);
        return;
    }
    consume_one(c, idx, in_belt);
    send_detailed_state(game, c); /*HP, inventory and hunger to the HUD at once*/
}

/*use <n|item> (quaff, read, eat, zap: the same command): a potion, food,
 * oil, a scroll or a wand from the backpack or the belt, or the magic of a
 * worn item ('use helm'). 'eat' alone looks for a ration. The ritual words
 * (study, pray, awaken, ...) learn from a book, inside the temple of the
 * class.*/
bool cmd_use(Client *c, const char *cmd, Game *game) {
    bool is_eat_cmd = (strcmp(cmd, "eat") == 0 || strncmp(cmd, "eat ", 4) == 0 ||
                       strcmp(cmd, "e") == 0 || strncmp(cmd, "e ", 2) == 0);
    const char *input = strchr(cmd, ' ');
    if (input) {
        input++;
    } else if (is_eat_cmd) {
        input = "ration"; /*'eat' alone looks for rations*/
    } else {
        send_text_to_client(c->sock, "[ERROR] Specify the subject (e.g. 'use 1' or 'study book').");
        return true;
    }
    const char *ritual = ritual_word_of(cmd);
    bool in_belt = false;
    int idx = is_eat_cmd ? find_food(c, input, &in_belt) : find_carried(c, input, &in_belt);
    ItemInstance *it = NULL;
    if (idx == -1 && is_eat_cmd) {
        send_text_to_client(c->sock, "[ERROR] You have nothing to eat.");
        return true;
    }
    if (idx == -1) {
        it = ritual ? NULL : find_equipped(c, input);
        if (!it || item_database[it->template_idx].cast_spell_idx < 0) {
            send_text_to_client(c->sock,
                                it ? "[ERROR] What you wear has no power to use."
                                   : "[ERROR] Item not found in inventory (use name or number).");
            return true;
        }
        use_magic_item(game, c, it, -1, false);
        return true;
    }
    it = in_belt ? &c->belt[idx] : &c->backpack[idx];
    ItemTemplate *tmpl = &item_database[it->template_idx];
    if (!it->is_identified) {
        send_text_to_client(c->sock, "[ERROR] You must first identify this object.");
        return true;
    }
    if (ritual && tmpl->category != ITEM_BOOK) {
        send_text_to_client(
            c->sock,
            "[ERROR] Ritual commands (like 'study') are only for learning from magical books.");
        return true;
    }
    if (tmpl->category == ITEM_BOOK) {
        learn_from_book(c, in_belt ? 1 : idx + 1, tmpl, ritual);
        return true;
    }
    if (tmpl->cast_spell_idx >= 0) {
        use_magic_item(game, c, it, idx, in_belt);
        return true;
    }
    use_consumable(game, c, idx, in_belt, tmpl);
    return true;
}
