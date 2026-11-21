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
 * cmd_character.c - Character commands: train, stats, rest, save.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "server_internal.h"
#include "player.h"
#include "inventory.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

//train <stat> or train random — awards unspent stat points
bool cmd_train(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *stat = cmd + 5;
    while (*stat == ' ') {
        stat++;
    }

    if (c->unspent_stat_points <= 0) {
        send_text_to_client(c->sock, "[SYSTEM] You have no stat points to assign.");
        return true;
    }
    if (strlen(stat) == 0) {
        send_text_to_client(
            c->sock,
            "[SYSTEM] Specify a stat (str, dex, con, int, wis, cha) or 'random'. You have %d points.",
            c->unspent_stat_points);
        return true;
    }
    /*The abbreviation or the full name, nothing else.*/
    static const char *const short_names[6] = {"str", "dex", "con", "int", "wis", "cha"};
    static const char *const long_names[6] = {"strength",     "dexterity", "constitution",
                                              "intelligence", "wisdom",    "charisma"};
    char word[16] = "";
    size_t wl = strcspn(stat, " ");
    /*One word (trailing spaces allowed) that fits; anything else matches nothing.*/
    if (wl < sizeof(word) && stat[wl + strspn(stat + wl, " ")] == '\0') {
        memcpy(word, stat, wl);
        word[wl] = '\0';
    }
    int chosen = -1; //0=str, 1=dex, 2=con, 3=int, 4=wis, 5=cha
    for (int k = 0; k < 6; k++) {
        if (strcasecmp(word, short_names[k]) == 0 || strcasecmp(word, long_names[k]) == 0) {
            chosen = k;
        }
    }
    if (strcasecmp(word, "random") == 0) {
        chosen = rand() % 6;
    }

    if (chosen == -1) {
        send_text_to_client(c->sock,
                            "[SYSTEM] Unknown stat. Use str, dex, con, int, wis, cha or random.");
        return true;
    }

    c->unspent_stat_points--;
    char *stat_name = "";
    /*rules_get_modifier(): floor((score-10)/2), not a division that
     * truncates toward zero (CON 9->10 changes the modifier).*/
    int old_mod = rules_get_modifier(c->con);

    if (chosen == 0) {
        c->str++;
        stat_name = "Strength";
    } else if (chosen == 1) {
        c->dex++;
        stat_name = "Dexterity";
    } else if (chosen == 2) {
        c->con++;
        stat_name = "Constitution";
        int new_mod = rules_get_modifier(c->con);
        if (new_mod > old_mod) {
            c->max_hp += c->level;
            c->hp += c->level;
            send_text_to_client(c->sock,
                                "[SYSTEM] Your Constitution makes you more resistant! Max HP +%d.",
                                c->level);
        }
    } else if (chosen == 3) {
        c->intel++;
        stat_name = "Intelligence";
    } else if (chosen == 4) {
        c->wis++;
        stat_name = "Wisdom";
    } else if (chosen == 5) {
        c->cha++;
        stat_name = "Charisma";
    }

    send_text_to_client(c->sock, "[SYSTEM] You have increased %s! (Points remaining: %d)",
                        stat_name, c->unspent_stat_points);
    save_player_data(c);
    send_detailed_state(game, c);
    return true;
}

bool cmd_stats(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    /*The armour class the monsters roll against.*/
    int ac = get_player_ac(c);
    int ac_base =
        (c->slot_body.template_idx != -1) ? item_database[c->slot_body.template_idx].ac_base : 0;
    //Weapon equipped
    const char *weapon_name = "Bare hands";
    char weapon_buf[128];
    if (c->slot_hand_r.template_idx != -1) {
        get_full_item_name(&c->slot_hand_r, weapon_buf, sizeof(weapon_buf));
        weapon_name = weapon_buf;
    } else if (c->slot_hand_l.template_idx != -1) {
        get_full_item_name(&c->slot_hand_l, weapon_buf, sizeof(weapon_buf));
        weapon_name = weapon_buf;
    }
    //Proficiency bonus: +2 at levels 1-4, +3 at 5-8, ... (as in combat)
    int prof = 2 + (c->level - 1) / 4;
    send_text_to_client(c->sock, "======== %s ========", c->username);
    send_text_to_client(c->sock, "Lvl:%-3d XP:%-6d Floor:%d", c->level, c->xp, c->floor_id);
    send_text_to_client(c->sock, "HP: %d/%d AC:%d Prof:+%d Gold:%lu gp", c->hp, c->max_hp, ac, prof,
                        (unsigned long)c->gold);
    send_text_to_client(c->sock, "--- ATTRIBUTES ---");
    send_text_to_client(c->sock, "  STR %2d(%+d)  DEX %2d(%+d)  CON %2d(%+d)", ts,
                        rules_get_modifier(ts), td, rules_get_modifier(td), tc,
                        rules_get_modifier(tc));
    send_text_to_client(c->sock, "  INT %2d(%+d)  WIS %2d(%+d)  CHA %2d(%+d)", ti,
                        rules_get_modifier(ti), tw, rules_get_modifier(tw), th,
                        rules_get_modifier(th));
    send_text_to_client(c->sock, "--- EQUIPMENT ---");
    send_text_to_client(c->sock, "  Weapon: %s", weapon_name);
    if (c->slot_body.template_idx != -1) {
        char armor_buf[128];
        get_full_item_name(&c->slot_body, armor_buf, sizeof(armor_buf));
        send_text_to_client(c->sock, "Armor: %s (base AC %d)", armor_buf, ac_base);
    }
    //Spell Slots
    bool has_slots = false;
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
        if (c->spell_slots_max[s] > 0) {
            has_slots = true;
            break;
        }
    }
    if (has_slots) {
        send_text_to_client(c->sock, "--- SPELL SLOTS ---");
        for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
            if (c->spell_slots_max[s] > 0) {
                send_text_to_client(c->sock, "  Lvl%d: %d/%d", s, c->spell_slots[s],
                                    c->spell_slots_max[s]);
            }
        }
    }
    //Conditions, then the other effects (potions, spells)
    if (c->effect_count > 0) {
        send_text_to_client(c->sock, "--- CONDITIONS AND EFFECTS ---");
        for (int e = 0; e < c->effect_count; e++) {
            const ActiveEffect *ef = &c->effects[e];
            bool condition = condition_from_name(ef->name) != MAX_CONDITIONS;
            char rounds[32] = "persistent";
            if (!ef->is_persistent) {
                snprintf(rounds, sizeof(rounds), "%d rounds", ef->duration_rounds);
            }
            send_text_to_client(c->sock, "  %s%s (%s)", ef->name ? ef->name : "Effect",
                                condition ? "" : " [effect]", rounds);
        }
    }
    send_text_to_client(c->sock, "==============================");
    return true;
}

/*A charged item carried in the backpack, on the belt or worn (a staff in
 * hand, a ring) gets all its charges back.*/
static int recharge_wands(Client *c) {
    int recharged = 0;
    int carried = c->backpack_count + MAX_BELT;
    for (int i = 0; i < carried + EQUIP_SLOT_COUNT + 10; i++) {
        ItemInstance *it = NULL;
        if (i < c->backpack_count) {
            it = &c->backpack[i];
        } else if (i < carried) {
            it = &c->belt[i - c->backpack_count];
        } else if (i < carried + EQUIP_SLOT_COUNT) {
            it = equip_slot(c, i - carried);
        } else {
            it = &c->slot_rings[i - carried - EQUIP_SLOT_COUNT];
        }
        if (item_recharges(it) &&
            it->current_charges < item_database[it->template_idx].max_charges) {
            it->current_charges = item_database[it->template_idx].max_charges;
            recharged++;
        }
    }
    return recharged;
}

/*While a character rests the merchants of the town restock (LORE.md,
 * "Merchant Restocking"). Returns how many shops did.*/
static int restock_town(Game *game) {
    int shops = 0;
    FloorIter walk = floor_iter(game, 0);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        NPC *n = &game->npcs[i];
        MerchantData *shop =
            n->active && n->archetype == ARCH_MERCHANT ? npc_shop(&game->pools, n) : NULL;
        if (shop) {
            merchant_restock(shop);
            shops++;
        }
    }
    return shops;
}

/*rest: in town, every hit point, spell slot and magic item charge back,
 * and the shops restocked.*/
bool cmd_rest(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    if (c->floor_id == 0) {
        c->hp = c->max_hp;
        for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
            c->spell_slots[s] = c->spell_slots_max[s];
        }
        int wands = recharge_wands(c);
        if (restock_town(game) > 0) {
            send_text_to_client(c->sock, "[SYSTEM] The merchants of the town restock.");
        }
        send_detailed_state(game, c); //Sync HP and spell slots with the HUD client
        if (wands > 0) {
            send_text_to_client(c->sock, "[SYSTEM] Rested. %d magic item%s recharged.", wands,
                                wands == 1 ? "" : "s");
        } else {
            send_text_to_client(c->sock, "[SYSTEM] Rested.");
        }
    } else {
        send_text_to_client(c->sock, "[SYSTEM] You can rest only in town (floor 0).");
    }
    return true;
}

bool cmd_save(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    save_player_data(c);
    send_text_to_client(c->sock, "[SYSTEM] Saved.");
    return true;
}
