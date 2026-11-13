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
 * cmd_shop.c - Merchant commands: list, browse, buy, sell, haggle, identify, cure,
 * repair, uncurse, talk.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "strnum.h"
#include "server_cmd.h"
#include "server_internal.h"
#include "server_world.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "inventory.h"
#include <ctype.h>

//browse <n> — inspect item description in store
bool cmd_browse(Client *c, const char *cmd, Game *game) {
    int target_item = list_index(cmd + 7);
    bool found = false;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 4) {
            if (target_item >= 0 && target_item < game->npcs[i].merchant.item_count) {
                ItemTemplate *it =
                    &item_database[game->npcs[i].merchant.item_templates[target_item]];
                send_text_to_client(c->sock, "[INFO] %s: %s", it->name, it->description);
                found = true;
            } else {
                send_text_to_client(c->sock, "[ERROR] Invalid object number.");
                found = true;
            }
            break;
        }
    }
    if (!found) {
        send_text_to_client(c->sock, "[SYSTEM] No merchants nearby.");
    }
    return true;
}

bool cmd_list(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    /*First check if there is a tombstone nearby*/
    bool tomb_found = false;
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        if (game->tombstones[i].active && game->tombstones[i].floor_id == c->floor_id &&
            abs(game->tombstones[i].x - c->x) + abs(game->tombstones[i].y - c->y) <= 1) {
            tombstone_list(game, c);
            tomb_found = true;
            break;
        }
    }
    if (tomb_found) {
        return true;
    }
    /*Then look for a merchant*/
    bool found = false;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 4) {
            print_merchant_inventory(c, &game->npcs[i]);
            found = true;
            break;
        }
    }
    if (!found) {
        send_text_to_client(c->sock, "[SYSTEM] No merchants or tombstones nearby.");
    }
    return true;
}

/*The merchant within 4 steps of the player, or NULL.*/
static NPC *nearby_merchant(const Client *c, Game *game) {
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 4) {
            return &game->npcs[i];
        }
    }
    return NULL;
}

/*The shelf of the merchant that holds the item template, or -1.*/
static int shop_shelf_of(const NPC *m, int template_idx) {
    for (int j = 0; j < m->merchant.item_count; j++) {
        if (m->merchant.item_templates[j] == template_idx) {
            return j;
        }
    }
    return -1;
}

/*Reactive economy: a shelf that is running out raises the price, up to
 * +50% when it is empty (1.0 for an item the merchant does not stock).*/
static float shop_scarcity(const NPC *m, int shelf) {
    if (shelf < 0 || m->merchant.item_stock_max[shelf] <= 0) {
        return 1.0f;
    }
    float supply = (float)m->merchant.item_stock[shelf] / (float)m->merchant.item_stock_max[shelf];
    float modifier = 1.0f + (1.0f - supply) * 0.5f;
    if (modifier < 0.5f) {
        return 0.5f;
    }
    return modifier > 2.0f ? 2.0f : modifier;
}

/*No bargain lets a player buy under SHOP_MIN_BUY_PCT of an item's cost or
 * sell over SHOP_MAX_SELL_PCT of it: with the first above the second,
 * buying and selling back never makes gold.*/
#define SHOP_MIN_BUY_PCT  80
#define SHOP_MAX_SELL_PCT 75

/*What the player pays for the item on the shelf, haggled by multiplier.*/
static uint64_t shop_buy_price(const NPC *m, int shelf, float multiplier) {
    uint64_t cost = item_database[m->merchant.item_templates[shelf]].cost;
    uint64_t price = (uint64_t)((float)cost * shop_scarcity(m, shelf) * multiplier);
    uint64_t floor_price = cost * SHOP_MIN_BUY_PCT / 100;
    return price < floor_price ? floor_price : price;
}

/*What the merchant pays for an item: half its cost, more if he is short
 * of it, haggled by multiplier.*/
static uint64_t shop_sell_value(const NPC *m, int template_idx, float multiplier) {
    uint64_t cost = item_database[template_idx].cost;
    uint64_t value = (uint64_t)((float)cost * 0.5f *
                                shop_scarcity(m, shop_shelf_of(m, template_idx)) * multiplier);
    uint64_t cap = cost * SHOP_MAX_SELL_PCT / 100;
    return value > cap ? cap : value;
}

/*A shelf by number ("3") or by a part of the item's name; -1 if none.*/
static int shop_find_shelf(const NPC *m, const char *input) {
    if (isdigit((unsigned char)input[0])) {
        int idx = list_index(input);
        return (idx >= 0 && idx < m->merchant.item_count) ? idx : -1;
    }
    for (int j = 0; input[0] && j < m->merchant.item_count; j++) {
        if (strcasestr(item_database[m->merchant.item_templates[j]].name, input) != NULL) {
            return j;
        }
    }
    return -1;
}

/*A backpack slot by number or by the exact name of the item; -1 if none.*/
static int backpack_find(const Client *c, const char *input) {
    if (isdigit((unsigned char)input[0])) {
        int idx = list_index(input);
        return (idx >= 0 && idx < c->backpack_count) ? idx : -1;
    }
    for (int i = 0; i < c->backpack_count; i++) {
        if (strcasecmp(input, item_database[c->backpack[i].template_idx].name) == 0) {
            return i;
        }
    }
    return -1;
}

/*True if the player haggled this very deal: same merchant, same side,
 * same slot and still the same item in it.*/
static bool haggled_deal(const Client *c, const NPC *m, bool buying, int idx, int template_idx) {
    return c->pending_trade_merchant_id == m->entity_id && c->pending_trade_is_buy == buying &&
           c->pending_trade_item_idx == idx && c->pending_trade_template_idx == template_idx;
}

/*One unit of the item into the backpack: on a stack with room, else in a
 * new slot. False when the backpack is full.*/
static bool backpack_add_bought(Client *c, int template_idx) {
    const ItemTemplate *it = &item_database[template_idx];
    for (int i = 0; it->max_stack > 1 && i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == template_idx &&
            c->backpack[i].stack_count < it->max_stack) {
            c->backpack[i].stack_count++;
            return true;
        }
    }
    if (c->backpack_count >= MAX_BACKPACK) {
        return false;
    }
    ItemInstance *slot = &c->backpack[c->backpack_count++];
    memset(slot, 0, sizeof(ItemInstance));
    slot->template_idx = template_idx;
    slot->durability = (it->category == ITEM_LIGHT_SOURCE && strcasestr(it->name, "lantern"))
                           ? 5000
                           : it->max_durability;
    slot->stack_count = 1;
    slot->is_identified = true; /*the merchant vouches for the item*/
    slot->blessing = BLESS_NORMAL;
    slot->quality = QUALITY_NORMAL;
    slot->element = ELEM_NONE;
    return true;
}

/*buy <n|name>: one unit from the merchant nearby, at the haggled price if
 * this deal was haggled.*/
bool cmd_buy(Client *c, const char *cmd, Game *game) {
    NPC *m = nearby_merchant(c, game);
    if (!m) {
        send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
        return true;
    }
    int shelf = shop_find_shelf(m, cmd + 4);
    if (shelf == -1) {
        send_text_to_client(c->sock, "[STORE] Item not found.");
        return true;
    }
    if (m->merchant.item_stock[shelf] <= 0) {
        send_text_to_client(c->sock, "[STORE] Item sold out!");
        return true;
    }
    int template_idx = m->merchant.item_templates[shelf];
    uint64_t final_cost = shop_buy_price(m, shelf, 1.0f);
    if (haggled_deal(c, m, true, shelf, template_idx)) {
        final_cost = c->pending_trade_price;
        send_text_to_client(c->sock, "[SYSTEM] Apply the negotiated price: %lu gp.",
                            (unsigned long)final_cost);
    }
    if (c->gold < final_cost) {
        send_text_to_client(c->sock, "[ERROR] Insufficient gold (%lu gp required).",
                            (unsigned long)final_cost);
        return true;
    }
    if (!backpack_add_bought(c, template_idx)) {
        send_text_to_client(c->sock, "[ERROR] Backpack full!");
        return true;
    }
    c->gold -= final_cost;
    m->merchant.item_stock[shelf]--;
    send_text_to_client(c->sock, "[STORE] Bought %s for %lu gp.", item_database[template_idx].name,
                        (unsigned long)final_cost);
    c->pending_trade_merchant_id = -1; /*a haggled price is good for one deal*/
    save_player_data(c);
    return true;
}

/*sell <n|name>: one unit to the merchant nearby, who puts it on his shelf
 * if he stocks it.*/
bool cmd_sell(Client *c, const char *cmd, Game *game) {
    NPC *m = nearby_merchant(c, game);
    if (!m) {
        send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
        return true;
    }
    int si = backpack_find(c, cmd + 5);
    if (si == -1) {
        send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
        return true;
    }
    int template_idx = c->backpack[si].template_idx;
    uint64_t final_val = shop_sell_value(m, template_idx, 1.0f);
    if (haggled_deal(c, m, false, si, template_idx)) {
        final_val = c->pending_trade_price;
        send_text_to_client(c->sock, "[SYSTEM] Apply the negotiated price: %lu gp.",
                            (unsigned long)final_val);
    }
    int shelf = shop_shelf_of(m, template_idx);
    if (shelf != -1) {
        m->merchant.item_stock[shelf]++;
    }
    c->gold += final_val;
    send_text_to_client(c->sock, "[STORE] Sold %s for %lu gp.", item_database[template_idx].name,
                        (unsigned long)final_val);
    if (c->backpack[si].stack_count > 1) {
        c->backpack[si].stack_count--;
    } else {
        c->backpack[si] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
    }
    c->pending_trade_merchant_id = -1;
    save_player_data(c);
    return true;
}

/*The duel of Charisma: d20 + CHA against the merchant's d20 + 2 + 3 for
 * every attempt already made on this deal. Returns the factor on the price
 * (buying) or on the value (selling).*/
static float haggle_roll(Client *c, bool buying) {
    int impatience = (c->pending_trade_attempts - 1) * 3;
    int p_roll = rules_roll_dice(1, 20);
    int m_roll = rules_roll_dice(1, 20) + 2 + impatience;
    int p_total = p_roll + rules_get_modifier(c->cha);
    send_text_to_client(c->sock, "> Your Roll: %d + %d = %d", p_roll, rules_get_modifier(c->cha),
                        p_total);
    send_text_to_client(c->sock, "> Merchant Roll: %d (Impatience: +%d)", m_roll - impatience,
                        impatience);
    if (p_roll == 20) {
        send_text_to_client(c->sock,
                            "[CRITICAL SUCCESS] The merchant is enraptured by your words!");
        return buying ? 0.5f : 1.75f;
    }
    if (p_roll == 1) {
        send_text_to_client(c->sock, "[CRITICAL FAIL] The merchant is mortally offended!");
        c->pending_trade_attempts += 5; /*much harder to recover from*/
        return buying ? 2.5f : 0.10f;
    }
    if (p_total >= m_roll) {
        send_text_to_client(c->sock, "[SUCCESS] You managed to get a better price.");
        return buying ? 0.8f : 1.25f;
    }
    send_text_to_client(c->sock,
                        "[FAILURE] The merchant does not give up. Maybe you should stop...");
    return buying ? 1.2f : 0.8f; /*the price gets worse*/
}

/*haggle buy <n> | haggle sell <n>: bargains the price of one deal; the
 * next 'buy <n>' or 'sell <n>' of that same item applies it.*/
bool cmd_haggle(Client *c, const char *cmd, Game *game) {
    const char *sub = cmd + 7;
    bool is_buying = (strncmp(sub, "buy ", 4) == 0);
    if (!is_buying && strncmp(sub, "sell ", 5) != 0) {
        send_text_to_client(c->sock, "[ERROR] Usage: haggle buy <n> or haggle sell <n>");
        return true;
    }
    const char *input = is_buying ? sub + 4 : sub + 5;
    NPC *m = nearby_merchant(c, game);
    if (!m) {
        send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
        return true;
    }
    int item_idx = -1;
    if (isdigit((unsigned char)input[0])) {
        item_idx = is_buying ? shop_find_shelf(m, input) : backpack_find(c, input);
    }
    if (is_buying && (item_idx == -1 || m->merchant.item_stock[item_idx] <= 0)) {
        send_text_to_client(c->sock, "[SHOP] Item not available for trading.");
        return true;
    }
    if (item_idx == -1) {
        send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
        return true;
    }
    int template_idx =
        is_buying ? m->merchant.item_templates[item_idx] : c->backpack[item_idx].template_idx;
    if (haggled_deal(c, m, is_buying, item_idx, template_idx)) {
        c->pending_trade_attempts++;
    } else {
        c->pending_trade_attempts = 1;
    }
    send_text_to_client(c->sock, "[GAME] You try to bargain with %s... (Attempt %d)",
                        m->merchant.shop_name, c->pending_trade_attempts);
    float multiplier = haggle_roll(c, is_buying);
    uint64_t price = is_buying ? shop_buy_price(m, item_idx, multiplier)
                               : shop_sell_value(m, template_idx, multiplier);
    c->pending_trade_item_idx = item_idx;
    c->pending_trade_template_idx = template_idx;
    c->pending_trade_price = price;
    c->pending_trade_merchant_id = m->entity_id;
    c->pending_trade_is_buy = is_buying;
    send_text_to_client(c->sock, "[STORE] Current offer for %s%s: %lu gp. Use '%s %d' to confirm.",
                        is_buying ? "" : "your ", item_database[template_idx].name,
                        (unsigned long)price, is_buying ? "buy" : "sell", item_idx + 1);
    return true;
}

//identify [n] — identifies the first unidentified object
//or the nth if specified. Cost: 100 gp per item.
bool cmd_identify(Client *c, const char *cmd, Game *game) {
    //Check if we are close to a merchant
    bool near_merchant = false;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 4) {
            near_merchant = true;
            break;
        }
    }
    if (!near_merchant) {
        send_text_to_client(c->sock, "[ERROR] You must be near a merchant to identify.");
        return true;
    }
    //Optional argument parsing: "identify 3" or "identify sword"
    int target_idx = -1;
    if (cmd[8] == ' ') {
        const char *arg = cmd + 9;
        if (isdigit(arg[0])) {
            int n = list_index(arg);
            if (n >= 0 && n < c->backpack_count && !c->backpack[n].is_identified) {
                target_idx = n;
            }
        } else {
            for (int i = 0; i < c->backpack_count; i++) {
                if (!c->backpack[i].is_identified) {
                    target_idx = i;
                }
                const char *iname = item_database[c->backpack[i].template_idx].name;
                if (strcasecmp(iname, arg) == 0) {
                    target_idx = i;
                    break;
                }
            }
        }
    } else {
        //Without argument: first unidentified
        for (int i = 0; i < c->backpack_count; i++) {
            if (!c->backpack[i].is_identified) {
                target_idx = i;
                break;
            }
        }
    }
    if (target_idx == -1) {
        send_text_to_client(c->sock, "[ALCHEMIST] You have no unidentified items.");
        return true;
    }
    if (c->gold < 100) {
        send_text_to_client(c->sock, "[ALCHEMIST] Identification service costs 100gp."
                                     "You don't have enough gold.");
        return true;
    }
    c->gold -= 100;
    c->backpack[target_idx].is_identified = true;
    char id_buf[128];
    get_full_item_name(&c->backpack[target_idx], id_buf, sizeof(id_buf));
    if (c->backpack[target_idx].blessing == BLESS_CURSED) {
        send_text_to_client(c->sock,
                            "[ALCHEMIST] *whistles softly* The item is: %s"
                            "-- WARNING: it's CURSED! Don't wear it!",
                            id_buf);
    } else {
        send_text_to_client(c->sock, "[ALCHEMIST] The item is: %s", id_buf);
    }
    return true;
}

bool cmd_cure(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    if (c->gold >= 50) {
        c->gold -= 50;
        c->effect_count = 0;
        send_text_to_client(c->sock,
                            "[TEMPLE] Your afflictions have been cleansed by the Arcanum!");
    } else {
        send_text_to_client(c->sock, "[ERROR] Not enough gold (50gp).");
    }
    return true;
}

//repair [slot|name] — repairs ONE equipped item or the slot.
//Must be done on floor 0 near the Blacksmith (NPC with name "Blacksmith"
//or ARCH_BLACKSMITH or any merchant on floor 0 as a fallback).
bool cmd_repair(Client *c, const char *cmd, Game *game) {
    if (c->floor_id != 0) {
        send_text_to_client(c->sock, "[ERROR] The Blacksmith is only found on floor 0 (city).");
        return true;
    }
    //Check for a merchant/blacksmith near floor 0
    bool near_smith = false;
    FloorIter walk = floor_iter(game, 0);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 5) {
            near_smith = true;
            break;
        }
    }
    if (!near_smith) {
        send_text_to_client(c->sock, "[ERROR] You must be near the Blacksmith to repair.");
        return true;
    }
    //Collects all repairable slots
    ItemInstance *sls[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                           &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                           &c->slot_arm_l,  &c->slot_feet};
    //Optional argument: specific slot
    const char *repair_arg = (cmd[6] == ' ') ? cmd + 7 : NULL;
    int total_cost = 0;
    int items_repaired = 0;
    for (int i = 0; i < 10; i++) {
        if (sls[i]->template_idx == -1) {
            continue;
        }
        //If an argument is specified, repair only that object
        if (repair_arg != NULL) {
            const char *iname = item_database[sls[i]->template_idx].name;
            if (strcasecmp(iname, repair_arg) != 0 &&
                !(sls[i]->is_artifact && strcasecmp(sls[i]->artifact_name, repair_arg) == 0)) {
                continue;
            }
        }
        int max_dur = item_database[sls[i]->template_idx].max_durability;
        if (max_dur <= 0) {
            continue;
        }
        int diff = max_dur - sls[i]->durability;
        if (diff <= 0) {
            continue;
        }
        //Cost: 5gp per point of durability lost; artifacts cost 10x
        int item_cost = diff * (sls[i]->is_artifact ? 50 : 5);
        if ((uint64_t)item_cost > c->gold) {
            char rep_buf[128];
            get_full_item_name(sls[i], rep_buf, sizeof(rep_buf));
            send_text_to_client(c->sock,
                                "[BLACKSMITH] You don't have enough gold to repair %s (%dgp).",
                                rep_buf, item_cost);
            continue;
        }
        c->gold -= (uint64_t)item_cost;
        sls[i]->durability = max_dur;
        //Restore the quality if it was Rusty due to wear
        if (sls[i]->quality == QUALITY_RUSTY) {
            sls[i]->quality = QUALITY_NORMAL;
        }
        total_cost += item_cost;
        items_repaired++;
        char rep_buf[128];
        get_full_item_name(sls[i], rep_buf, sizeof(rep_buf));
        send_text_to_client(c->sock, "[BLACKSMITH] Fixed: %s for %d gp. Durab. now %d/%d.", rep_buf,
                            item_cost, sls[i]->durability, max_dur);
    }
    if (items_repaired == 0 && total_cost == 0) {
        send_text_to_client(c->sock,
                            "[BLACKSMITH] Your items are in excellent shape, there is nothing"
                            "to repair (or you don't have enough gold).");
    } else {
        send_text_to_client(c->sock, "[BLACKSMITH] Work completed. Total spent: %d gp.",
                            total_cost);
    }
    return true;
}

//uncurse — ask a cleric to remove a curse.
//Cost: 200gp. Requires merchant near floor 0.
bool cmd_uncurse(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    if (c->floor_id != 0) {
        send_text_to_client(c->sock, "[ERROR] The Cleric is only on floor 0 (city).");
        return true;
    }
    bool near_cleric = false;
    FloorIter walk = floor_iter(game, 0);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 4) {
            near_cleric = true;
            break;
        }
    }
    if (!near_cleric) {
        send_text_to_client(c->sock, "[ERROR] You must be near the Cleric to remove"
                                     "to curse.");
        return true;
    }
    if (c->gold < 200) {
        send_text_to_client(c->sock, "[CLERIC] The purification rite costs 200gp."
                                     "You don't have enough gold.");
        return true;
    }
    //Remove the curse from ALL equipped slots
    ItemInstance *sls2[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                            &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                            &c->slot_arm_l,  &c->slot_feet};
    int uncursed_count = 0;
    for (int i = 0; i < 10; i++) {
        if (sls2[i]->template_idx != -1 && sls2[i]->blessing == BLESS_CURSED) {
            sls2[i]->blessing = BLESS_NORMAL;
            uncursed_count++;
        }
    }
    for (int ri = 0; ri < 10; ri++) {
        if (c->slot_rings[ri].template_idx != -1 && c->slot_rings[ri].blessing == BLESS_CURSED) {
            c->slot_rings[ri].blessing = BLESS_NORMAL;
            uncursed_count++;
        }
    }
    if (uncursed_count == 0) {
        send_text_to_client(c->sock, "[CLERIC] Do not carry cursed objects. Save your 200gp.");
        return true;
    }
    c->gold -= 200;
    send_text_to_client(c->sock,
                        "[CLERIC] The sacred fire burns the curses! %d item(s)."
                        "purified(s) (200gp).",
                        uncursed_count);
    return true;
}

bool cmd_talk(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_MERCHANT &&
            abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 2) {
            send_text_to_client(
                c->sock,
                "[%s] Welcome! Commands: 'list', 'browse <n>', 'buy <n>', 'sell <n>',"
                " 'haggle buy/sell', 'identify [n]', 'repair [name]', 'uncurse', 'cure'.",
                game->npcs[i].merchant.shop_name);
            print_merchant_inventory(c, &game->npcs[i]);
            return true;
        }
    }
    send_text_to_client(c->sock, "[SYSTEM] There is no merchant within 2 steps.");
    return true;
}

void print_merchant_inventory(Client *c, NPC *merchant) {
    send_text_to_client(c->sock, ".----------------------------------------------"
                                 "----------------------------.");
    char title[128];
    snprintf(title, sizeof(title), "=== %s ===", merchant->merchant.shop_name);
    int pad = (74 - (int)strlen(title)) / 2;
    send_text_to_client(c->sock, "|%*s%s%*s|", pad, "", title, 74 - pad - (int)strlen(title), "");
    send_text_to_client(c->sock, "|----------------------------------------------"
                                 "----------------------------|");
    send_text_to_client(c->sock, "| ID | %-24s | %-10s | %-15s | %-9s |", "ITEM", "TYPE",
                        "STATISTICS", "COST");
    send_text_to_client(c->sock, "|----|--------------------------|------------|-"
                                 "----------------|-----------|");
    for (int j = 0; j < merchant->merchant.item_count; j++) {
        ItemTemplate *it = &item_database[merchant->merchant.item_templates[j]];
        char stat_info[32] = "-";
        char type_info[16] = "Misc";

        if (it->category == ITEM_WEAPON) {
            strcpy(type_info, "Weapon");
            snprintf(stat_info, sizeof(stat_info), "%dd%d dmg", it->damage_dice_count,
                     it->damage_dice_sides);
        } else if (it->category == ITEM_ARMOR) {
            strcpy(type_info, "Armor");
            snprintf(stat_info, sizeof(stat_info), "AC %d", it->ac_base);
        } else if (it->category == ITEM_SHIELD) {
            strcpy(type_info, "Shield");
            snprintf(stat_info, sizeof(stat_info), "+%d AC", it->ac_bonus);
        } else if (it->category == ITEM_CONSUMABLE) {
            strcpy(type_info, "Consum.");
            if (it->heal_amount > 0) {
                snprintf(stat_info, sizeof(stat_info), "Heal %d HP", it->heal_amount);
            }
        } else if (it->category == ITEM_RING) {
            strcpy(type_info, "Ring");
        } else if (it->category == ITEM_NECK) {
            strcpy(type_info, "Amulet");
        } else if (it->category == ITEM_LIGHT_SOURCE) {
            strcpy(type_info, "Light");
            snprintf(stat_info, sizeof(stat_info), "%d turns",
                     it->max_durability > 0 ? it->max_durability : 100);
        } else if (it->category == ITEM_FUEL) {
            strcpy(type_info, "Fuel");
            snprintf(stat_info, sizeof(stat_info), "%d turns",
                     it->duration_turns > 0 ? it->duration_turns : 300);
        }

        if (it->str_bonus > 0 || it->dex_bonus > 0 || it->con_bonus > 0 || it->int_bonus > 0 ||
            it->wis_bonus > 0 || it->cha_bonus > 0) {
            if (strcmp(stat_info, "-") == 0) {
                copy_str(stat_info, "Magic", sizeof(stat_info));
            } else {
                strncat(stat_info, " (*)", sizeof(stat_info) - strlen(stat_info) - 1);
            }
        }

        /* --- Reactive economy (dynamic prices) --- */
        float supply = 1.0f;
        if (merchant->merchant.item_stock_max[j] > 0) {
            supply = (float)merchant->merchant.item_stock[j] /
                     (float)merchant->merchant.item_stock_max[j];
        }
        //+50% price if empty stock, up to -50% if double stock
        float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
        if (economy_modifier < 0.5f) {
            economy_modifier = 0.5f;
        }
        if (economy_modifier > 2.0f) {
            economy_modifier = 2.0f;
        }
        uint64_t dynamic_cost = (uint64_t)((float)it->cost * economy_modifier);

        char cost_str[16];
        snprintf(cost_str, sizeof(cost_str), "%lu gp", (unsigned long)dynamic_cost);

        send_text_to_client(c->sock, "| %2d | %-24.24s | %-10s | %-15.15s | %9s |", j + 1, it->name,
                            type_info, stat_info, cost_str);
    }
    send_text_to_client(c->sock, "'----------------------------------------------"
                                 "----------------------------'");
    send_text_to_client(c->sock, "[TIP] Use 'browse <n>' for info, 'buy <n>' to purchase.");
}
