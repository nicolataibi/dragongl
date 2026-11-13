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
#include "player.h"
#include <math.h>
#include <ctype.h>

bool cmd_retrieve(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    int ghost_npc_idx = -1;
    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "Ghost of %s", c->username);

    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].is_ghost && abs(game->npcs[i].x - c->x) <= 1 &&
            abs(game->npcs[i].y - c->y) <= 1) {
            if (strcmp(game->npcs[i].custom_name, expected_name) == 0) {
                ghost_npc_idx = i;
                break;
            }
        }
    }

    if (ghost_npc_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] No ghosts of you nearby.");
        return true;
    }

    if (game->npcs[ghost_npc_idx].gold_drop > 0) {
        c->gold += (uint64_t)game->npcs[ghost_npc_idx].gold_drop;
        game->npcs[ghost_npc_idx].gold_drop = 0;
    }
    for (int gi = 0; gi < 30; gi++) {
        if (game->npcs[ghost_npc_idx].ghost_loot[gi].template_idx >= 0 &&
            game->npcs[ghost_npc_idx].ghost_loot[gi].stack_count > 0 &&
            c->backpack_count < MAX_BACKPACK) {
            c->backpack[c->backpack_count++] = game->npcs[ghost_npc_idx].ghost_loot[gi];
            game->npcs[ghost_npc_idx].ghost_loot[gi].template_idx = -1;
            game->npcs[ghost_npc_idx].ghost_loot[gi].stack_count = 0;
        }
    }

    send_text_to_client(c->sock, "[SYSTEM] You have recovered your soul and equipment!");
    npc_set_active(game, &game->npcs[ghost_npc_idx], false);
    game->npcs[ghost_npc_idx].respawn_timer = 0;
    game->world->floors[c->floor_id]
        .entity_grid[game->npcs[ghost_npc_idx].y][game->npcs[ghost_npc_idx].x] = 0;
    save_player_data(c);
    return true;
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
                strcasecmp(item_database[c->backpack[i].template_idx].name, input) == 0) {
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
                int required_pg_level = global_idx / 3;
                send_text_to_client(c->sock, "  [#%2d] %-25s (Req. Lv PG: %d)", global_idx,
                                    tmpl->book_spell_names[local_idx], required_pg_level);
            }
        } else {
            send_text_to_client(c->sock, "(No readable spells listed here)");
        }
        send_text_to_client(c->sock, "--------------------------------");
    }
    return true;
}

//fill <n> - fills a lantern using an oil (fuel) from the backpack
bool cmd_fill(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = cmd + 5;
    int lantern_idx = -1;
    bool in_belt = false;
    if (isdigit(input[0])) {
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

    ItemInstance *target = in_belt ? &c->belt[lantern_idx] : &c->backpack[lantern_idx];
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

    fuel->stack_count--;
    if (fuel->stack_count <= 0) {
        c->backpack[fuel_idx] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
    }
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

/*One spell of the book: learned if the class may cast it and the
 * character has twice its level (cantrips: level 1).*/
static void study_spell(Client *c, const char *spell_name, StudyTally *tally) {
    int si = -1;
    for (int k = 0; k < spell_database_size && si < 0; k++) {
        if (strcasecmp(spell_database[k].name, spell_name) == 0) {
            si = k;
        }
    }
    if (si < 0 || !(spell_database[si].class_mask & (1u << (int)c->class_id))) {
        return;
    }
    const SpellTemplate *sp = &spell_database[si];
    int required_level = (sp->level > 0) ? (sp->level * 2) : 1;
    if (c->level < required_level) {
        send_text_to_client(c->sock, "  [-] %-30s  (requires PC Lv %d — LOCKED)", sp->name,
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
                            "[RITUAL] You have learned %d new magic(s)! Use 'spells' to "
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

/*Learns the spells of a book. The player must use the ritual command of
 * his class (study, pray, awaken, ...) inside the temple of his class on
 * floor 0, and learns the spells his level allows. The book is never
 * consumed: a spell is cast from its book, so it must be kept.*/
static void learn_from_book(Client *c, int found_idx, ItemTemplate *tmpl, bool is_ritual_cmd) {
    if (!is_ritual_cmd) {
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
    bool in_temple = (c->floor_id == 0 && c->x >= temple->x0 && c->x <= temple->x1 &&
                      c->y >= temple->y0 && c->y <= temple->y1);
    if (!in_temple) {
        send_text_to_client(c->sock,
                            "[RITUAL] To activate '%s' you must go to floor 0, in %s, "
                            "and use the command '%s %d' while you are physically inside.",
                            tmpl->name, temple->name, temple->ritual, found_idx + 1);
        return;
    }
    send_text_to_client(c->sock, "[RITUAL] %s... '%s' (you are in %s, Lv PG %d):", temple->ritual,
                        tmpl->name, temple->name, c->level);
    StudyTally tally = {0, 0, 0};
    for (int i = 0; i < tmpl->book_spell_count; i++) {
        study_spell(c, tmpl->book_spell_names[i], &tally);
    }
    study_summary(c, &tally);
    c->needs_study = false;
}

/*True for the ritual commands that learn from a book (one per class).*/
static bool is_ritual_command(const char *cmd) {
    static const char *const k_rituals[] = {"study",  "pray",      "awaken",    "invoke",
                                            "intone", "celebrate", "pronounce", "practice",
                                            "drill",  "rage",      "sneak",     "meditate"};
    for (size_t i = 0; i < sizeof(k_rituals) / sizeof(k_rituals[0]); i++) {
        if (strncmp(cmd, k_rituals[i], strlen(k_rituals[i])) == 0) {
            return true;
        }
    }
    return false;
}

/*The backpack slot of the item to use (a belt item moves to the backpack
 * to be consumed); -1 after the message when there is none.*/
static int use_find_item(Client *c, const char *input) {
    bool in_belt = false;
    int idx = find_carried(c, input, &in_belt);
    if (idx == -1) {
        send_text_to_client(c->sock, "[ERROR] Item not found in inventory (use name or number).");
        return -1;
    }
    if (!in_belt) {
        return idx;
    }
    if (c->backpack_count >= MAX_BACKPACK) {
        send_text_to_client(c->sock, "[ERROR] Backpack too full to handle the belt item!");
        return -1;
    }
    c->backpack[c->backpack_count] = c->belt[idx];
    c->belt[idx].template_idx = -1;
    return c->backpack_count++;
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

static void use_healing(Game *game, Client *c, const ItemTemplate *tmpl) {
    int healed = tmpl->heal_amount;
    c->hp = c->hp + healed > c->max_hp ? c->max_hp : c->hp + healed;
    if (strcasestr(tmpl->name, "Ration") != NULL) {
        c->hunger_level = 0;
        send_text_to_client(c->sock, "[ITEM] You ate %s. Hunger fully satisfied! (+%d HP).",
                            tmpl->name, healed);
        return;
    }
    item_use_vfx(game, c, tmpl->name);
    send_text_to_client(c->sock, "[ITEM] You used %s and recovered %d HP (HP: %d/%d).", tmpl->name,
                        healed, c->hp, c->max_hp);
}

/*A consumable that does not heal leaves a +2 effect for its duration.*/
static void use_effect_item(Game *game, Client *c, const ItemTemplate *tmpl) {
    item_use_vfx(game, c, tmpl->name);
    ActiveEffect ef;
    memset(&ef, 0, sizeof(ef));
    ef.name = tmpl->name;
    ef.trigger = EVENT_ON_TURN_START;
    ef.mod_type = MOD_ADDITIVE;
    ef.value = 2;
    ef.duration_rounds = tmpl->duration_turns > 0 ? tmpl->duration_turns : 10;
    if (rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &ef)) {
        send_text_to_client(c->sock, "[ITEM] You used %s. Effect active for %d rounds.", tmpl->name,
                            ef.duration_rounds);
    } else {
        send_text_to_client(c->sock,
                            "[ITEM] You used %s, but your magic-saturated body dispels the effect.",
                            tmpl->name);
    }
}

/*use <name|n>, eat [name]: a consumable or fuel from the backpack or the
 * belt. The ritual commands (study, pray, awaken, ...) are the same
 * command for books: they work only inside the temple of the class.*/
bool cmd_use(Client *c, const char *cmd, Game *game) {
    (void)game;
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
    bool is_ritual_cmd = is_ritual_command(cmd);
    int found_idx = use_find_item(c, input);
    if (found_idx == -1) {
        return true;
    }
    ItemTemplate *tmpl = &item_database[c->backpack[found_idx].template_idx];
    if (!c->backpack[found_idx].is_identified) {
        send_text_to_client(c->sock, "[ERROR] You must first identify this object.");
        return true;
    }
    if (is_ritual_cmd && tmpl->category != ITEM_BOOK) {
        send_text_to_client(
            c->sock,
            "[ERROR] Ritual commands (like 'study') are only for learning from magical books.");
        return true;
    }
    if (tmpl->category == ITEM_BOOK) {
        learn_from_book(c, found_idx, tmpl, is_ritual_cmd);
        return true;
    }
    if (tmpl->category != ITEM_CONSUMABLE && tmpl->category != ITEM_FUEL) {
        send_text_to_client(c->sock, "[ERROR] %s is not consumable or usable as fuel.", tmpl->name);
        return true;
    }
    if (tmpl->category == ITEM_FUEL) {
        if (!use_fuel(c, tmpl)) {
            return true;
        }
    } else if (tmpl->heal_amount > 0) {
        use_healing(game, c, tmpl);
    } else {
        use_effect_item(game, c, tmpl);
    }
    //Remove a unit from your backpack
    c->backpack[found_idx].stack_count--;
    if (c->backpack[found_idx].stack_count <= 0) {
        c->backpack[found_idx] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
    }
    send_detailed_state(game,
                        c); //Instantly sync HP, inventory and Vitality (hunger) with the HUD client
    return true;
}
