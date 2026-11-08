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

#include "server_cmd.h"
#include "server_internal.h"
#include "server_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

//browse <n> — inspect item description in store
bool cmd_browse(Client *c, const char *cmd, NPC *npcs) {
  int target_item = atoi(cmd + 7) - 1;
  bool found = false;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
      if (target_item >= 0 && target_item < npcs[i].merchant.item_count) {
        ItemTemplate *it =
            &item_database[npcs[i].merchant.item_templates[target_item]];
        send_text_to_client(c->sock, "[INFO] %s: %s", it->name,
                            it->description);
        found = true;
      } else {
        send_text_to_client(c->sock, "[ERROR] Invalid object number.");
        found = true;
      }
      break;
    }
  }
  if (!found)
    send_text_to_client(c->sock,
                        "[SYSTEM] No merchants nearby.");
  return true;
}

bool cmd_list(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  /*First check if there is a tombstone nearby*/
  bool tomb_found = false;
  for (int i = 0; i < MAX_TOMBSTONES; i++) {
    if (g_tombstones[i].active &&
        g_tombstones[i].floor_id == c->floor_id &&
        abs(g_tombstones[i].x - c->x) + abs(g_tombstones[i].y - c->y) <= 1) {
      tombstone_list(c);
      tomb_found = true;
      break;
    }
  }
  if (tomb_found) return true;
  /*Then look for a merchant*/
  bool found = false;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
      print_merchant_inventory(c, &npcs[i]);
      found = true;
      break;
    }
  }
  if (!found)
    send_text_to_client(c->sock,
                        "[SYSTEM] No merchants or tombstones nearby.");
  return true;
}

bool cmd_buy(Client *c, const char *cmd, NPC *npcs) {
  const char *input = cmd + 4;
  //1. Look for a nearby merchant
  int m_idx = -1;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT) {
      if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        m_idx = i;
        break;
      }
    }
  }

  if (m_idx == -1) {
    send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
    return true;
  }

  //2. Search for the object by name or index
  int shop_item_idx = -1;
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 &&
        requested_idx < npcs[m_idx].merchant.item_count) {
      shop_item_idx = requested_idx;
    }
  } else {
    for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
      int t_idx = npcs[m_idx].merchant.item_templates[j];
      if (strcasestr(item_database[t_idx].name, input) != NULL) {
        shop_item_idx = j;
        break;
      }
    }
  }

  if (shop_item_idx == -1) {
    send_text_to_client(c->sock, "[STORE] Item not found.");
    return true;
  }

  if (npcs[m_idx].merchant.item_stock[shop_item_idx] <= 0) {
    send_text_to_client(c->sock, "[STORE] Item sold out!");
    return true;
  }

  int d_idx = npcs[m_idx].merchant.item_templates[shop_item_idx];
  const ItemTemplate *it = &item_database[d_idx];

  /* --- Phase 3: Reactive Economy (Dynamic Prices) --- */
  float supply = 1.0f;
  if (npcs[m_idx].merchant.item_stock_max[shop_item_idx] > 0) {
    supply = (float)npcs[m_idx].merchant.item_stock[shop_item_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_item_idx];
  }
  float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
  if (economy_modifier < 0.5f) economy_modifier = 0.5f;
  if (economy_modifier > 2.0f) economy_modifier = 2.0f;
  uint64_t final_cost = (uint64_t)(it->cost * economy_modifier);

  //Check if there is a negotiated offer
  if (c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
      c->pending_trade_is_buy && c->pending_trade_item_idx == shop_item_idx) {
    final_cost = c->pending_trade_price;
    send_text_to_client(c->sock,
                        "[SYSTEM] Apply the negotiated price: %lu gp.",
                        (unsigned long)final_cost);
  }

  if (c->gold < final_cost) {
    send_text_to_client(c->sock,
                        "[ERROR] Insufficient gold (%lu gp required).",
                        (unsigned long)final_cost);
    return true;
  }

  //3. Backpack management (stacking or new slot)
  int backpack_idx = -1;
  if (it->max_stack > 1) {
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx == d_idx &&
          c->backpack[i].stack_count < it->max_stack) {
        backpack_idx = i;
        break;
      }
    }
  }

  if (backpack_idx != -1) {
    //Existing stack
    c->gold -= final_cost;
    c->backpack[backpack_idx].stack_count++;
    npcs[m_idx].merchant.item_stock[shop_item_idx]--;
    send_text_to_client(
        c->sock, "[STORE] Bought %s (added to stack).", it->name);
  } else if (c->backpack_count < MAX_BACKPACK) {
    //New slots
    c->gold -= final_cost;
    memset(&c->backpack[c->backpack_count], 0, sizeof(ItemInstance));
    c->backpack[c->backpack_count].template_idx  = d_idx;
    c->backpack[c->backpack_count].durability    = (it->category == ITEM_LIGHT_SOURCE && strcasestr(it->name, "lantern")) ? 5000 : it->max_durability;
    c->backpack[c->backpack_count].stack_count   = 1;
    c->backpack[c->backpack_count].is_identified = true; //Merchant guarantees the item
    c->backpack[c->backpack_count].blessing      = BLESS_NORMAL;
    c->backpack[c->backpack_count].quality       = QUALITY_NORMAL;
    c->backpack[c->backpack_count].element       = ELEM_NONE;
    c->backpack_count++;
    npcs[m_idx].merchant.item_stock[shop_item_idx]--;
    send_text_to_client(c->sock, "[STORE] Bought %s.", it->name);
  } else {
    send_text_to_client(c->sock, "[ERROR] Backpack full!");
  }
  save_player_data(c);

  //Reset offer after use
  c->pending_trade_merchant_id = -1;
  return true;
}

bool cmd_sell(Client *c, const char *cmd, NPC *npcs) {
  const char *input = cmd + 5;
  int si = -1;
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count)
      si = requested_idx;
  } else {
    for (int i = 0; i < c->backpack_count; i++)
      if (strcasecmp(input,
                     item_database[c->backpack[i].template_idx].name) == 0) {
        si = i;
        break;
      }
  }

  if (si != -1) {
    uint64_t final_val = item_database[c->backpack[si].template_idx].cost / 2;
    int m_idx = -1;
    FloorIter walk = floor_iter(npcs, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
      if (npcs[i].archetype == ARCH_MERCHANT) {
        if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
          m_idx = i;
          break;
        }
      }
    }

    if (m_idx != -1) {
      /* -- Phase 3: Reactive Economy (Selling) --- */
      int shop_idx = -1;
      for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
        if (npcs[m_idx].merchant.item_templates[j] == c->backpack[si].template_idx) {
          shop_idx = j;
          break;
        }
      }
      if (shop_idx != -1) {
        float supply = 1.0f;
        if (npcs[m_idx].merchant.item_stock_max[shop_idx] > 0) {
          supply = (float)npcs[m_idx].merchant.item_stock[shop_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_idx];
        }
        float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
        if (economy_modifier < 0.5f) economy_modifier = 0.5f;
        if (economy_modifier > 2.0f) economy_modifier = 2.0f;
        final_val = (uint64_t)(final_val * economy_modifier);

        /*The merchant acquires the item, increasing local supply*/
        npcs[m_idx].merchant.item_stock[shop_idx]++;
      }
    }

    if (m_idx != -1 &&
        c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
        !c->pending_trade_is_buy && c->pending_trade_item_idx == si) {
      final_val = c->pending_trade_price;
      send_text_to_client(c->sock,
                          "[SYSTEM] Apply the negotiated price: %lu gp.",
                          (unsigned long)final_val);
    }
    c->gold += final_val;
    send_text_to_client(c->sock, "[STORE] Sold %s for %lu gp.",
                        item_database[c->backpack[si].template_idx].name,
                        (unsigned long)final_val);
    if (c->backpack[si].stack_count > 1) {
      c->backpack[si].stack_count--;
    } else {
      c->backpack[si] = c->backpack[c->backpack_count - 1];
      c->backpack_count--;
    }
    save_player_data(c);
    c->pending_trade_merchant_id = -1;
  }
  return true;
}

bool cmd_haggle(Client *c, const char *cmd, NPC *npcs) {
  const char *sub = cmd + 7;
  bool is_buying = (strncmp(sub, "buy ", 4) == 0);
  bool is_selling = (strncmp(sub, "sell ", 5) == 0);

  if (!is_buying && !is_selling) {
    send_text_to_client(c->sock,
                        "[ERROR] Usage: haggle buy <n> or haggle sell <n>");
    return true;
  }

  const char *input = is_buying ? sub + 4 : sub + 5;
  int m_idx = -1;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT) {
      if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        m_idx = i;
        break;
      }
    }
  }
  if (m_idx == -1) {
    send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
    return true;
  }

  int item_idx = -1;
  if (is_buying) {
    if (isdigit(input[0])) {
      int r = atoi(input) - 1;
      if (r >= 0 && r < npcs[m_idx].merchant.item_count)
        item_idx = r;
    }
    if (item_idx == -1 || npcs[m_idx].merchant.item_stock[item_idx] <= 0) {
      send_text_to_client(
          c->sock, "[SHOP] Item not available for trading.");
      return true;
    }
  } else {
    if (isdigit(input[0])) {
      int r = atoi(input) - 1;
      if (r >= 0 && r < c->backpack_count)
        item_idx = r;
    }
    if (item_idx == -1) {
      send_text_to_client(c->sock,
                          "[ERROR] Item not found in backpack.");
      return true;
    }
  }

  //THE GAME: Duel of Charisma
  if (c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
      c->pending_trade_item_idx == item_idx &&
      c->pending_trade_is_buy == is_buying) {
    c->pending_trade_attempts++;
  } else {
    c->pending_trade_attempts = 1;
  }

  send_text_to_client(
      c->sock, "[GAME] You try to bargain with %s... (Attempt %d)",
      npcs[m_idx].merchant.shop_name, c->pending_trade_attempts);
  int p_roll = rules_roll_dice(1, 20);
  int m_impazienza = (c->pending_trade_attempts - 1) * 3;
  int m_roll = rules_roll_dice(1, 20) + 2 +
               m_impazienza; //The merchants are cunning and lose patience
  int p_total = p_roll + rules_get_modifier(c->cha);

  send_text_to_client(c->sock, "> Your Roll: %d + %d = %d", p_roll,
                      rules_get_modifier(c->cha), p_total);
  send_text_to_client(c->sock, "> Merchant Roll: %d (Impatience: +%d)",
                      m_roll - m_impazienza, m_impazienza);

  float multiplier = 1.0f;
  if (p_roll == 20) {
    send_text_to_client(
        c->sock,
        "[CRITICAL SUCCESS] The merchant is enraptured by your words!");
    multiplier = is_buying ? 0.5f : 1.75f;
  } else if (p_roll == 1) {
    send_text_to_client(
        c->sock, "[CRITICAL FAIL] The merchant is mortally offended!");
    multiplier = is_buying ? 2.5f : 0.10f; // Terrible prices!
    c->pending_trade_attempts += 5;        // Much harder to recover from
  } else if (p_total >= m_roll) {
    send_text_to_client(
        c->sock, "[SUCCESS] You managed to get a better price.");
    multiplier = is_buying ? 0.8f : 1.25f;
  } else {
    send_text_to_client(
        c->sock,
        "[FAILURE] The merchant does not give up. Maybe you should stop...");
    multiplier = is_buying ? 1.2f : 0.8f; //The price gets worse!
  }

  if (is_buying) {
    int d_idx = npcs[m_idx].merchant.item_templates[item_idx];
    /* --- Phase 3: Reactive Economy --- */
    float supply = 1.0f;
    if (npcs[m_idx].merchant.item_stock_max[item_idx] > 0) {
      supply = (float)npcs[m_idx].merchant.item_stock[item_idx] / (float)npcs[m_idx].merchant.item_stock_max[item_idx];
    }
    float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
    if (economy_modifier < 0.5f) economy_modifier = 0.5f;
    if (economy_modifier > 2.0f) economy_modifier = 2.0f;

    uint64_t final_cost =
        (uint64_t)((float)item_database[d_idx].cost * economy_modifier * multiplier);
    c->pending_trade_item_idx = item_idx;
    c->pending_trade_price = final_cost;
    c->pending_trade_merchant_id = npcs[m_idx].entity_id;
    c->pending_trade_is_buy = true;
    send_text_to_client(c->sock,
                        "[STORE] Current offer for %s: %lu gp. Use 'buy"
                        "%d' to confirm.",
                        item_database[d_idx].name, (unsigned long)final_cost,
                        item_idx + 1);
  } else {
    int d_idx = c->backpack[item_idx].template_idx;
    /* --- Phase 3: Reactive Economy --- */
    float economy_modifier = 1.0f;
    int shop_idx = -1;
    for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
      if (npcs[m_idx].merchant.item_templates[j] == d_idx) {
        shop_idx = j;
        break;
      }
    }
    if (shop_idx != -1) {
      float supply = 1.0f;
      if (npcs[m_idx].merchant.item_stock_max[shop_idx] > 0) {
        supply = (float)npcs[m_idx].merchant.item_stock[shop_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_idx];
      }
      economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
      if (economy_modifier < 0.5f) economy_modifier = 0.5f;
      if (economy_modifier > 2.0f) economy_modifier = 2.0f;
    }

    uint64_t final_val =
        (uint64_t)((float)(item_database[d_idx].cost / 2) * economy_modifier * multiplier);
    c->pending_trade_item_idx = item_idx;
    c->pending_trade_price = final_val;
    c->pending_trade_merchant_id = npcs[m_idx].entity_id;
    c->pending_trade_is_buy = false;
    send_text_to_client(c->sock,
                        "[STORE] Current offer for your %s: %lu gp."
                        "Use 'sell %d' to confirm.",
                        item_database[d_idx].name, (unsigned long)final_val,
                        item_idx + 1);
  }
  return true;
}

//identify [n] — identifies the first unidentified object
//or the nth if specified. Cost: 100 gp per item.
bool cmd_identify(Client *c, const char *cmd, NPC *npcs) {
  //Check if we are close to a merchant
  bool near_merchant = false;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
      near_merchant = true;
      break;
    }
  }
  if (!near_merchant) {
    send_text_to_client(c->sock,
        "[ERROR] You must be near a merchant to identify.");
    return true;
  }
  //Optional argument parsing: "identify 3" or "identify sword"
  int target_idx = -1;
  if (cmd[8] == ' ') {
    const char *arg = cmd + 9;
    if (isdigit(arg[0])) {
      int n = atoi(arg) - 1;
      if (n >= 0 && n < c->backpack_count && !c->backpack[n].is_identified)
        target_idx = n;
    } else {
      for (int i = 0; i < c->backpack_count; i++) {
        if (!c->backpack[i].is_identified)
          target_idx = i;
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
    send_text_to_client(c->sock,
        "[ALCHEMIST] You have no unidentified items.");
    return true;
  }
  if (c->gold < 100) {
    send_text_to_client(c->sock,
        "[ALCHEMIST] Identification service costs 100gp."
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
    send_text_to_client(c->sock,
        "[ALCHEMIST] The item is: %s", id_buf);
  }
  return true;
}

bool cmd_cure(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->gold >= 50) {
    c->gold -= 50;
    c->effect_count = 0;
    send_text_to_client(
        c->sock,
        "[TEMPLE] Your afflictions have been cleansed by the Arcanum!");
  } else
    send_text_to_client(c->sock, "[ERROR] Not enough gold (50gp).");
  return true;
}

//repair [slot|name] — repairs ONE equipped item or the slot.
//Must be done on floor 0 near the Blacksmith (NPC with name "Blacksmith"
//or ARCH_BLACKSMITH or any merchant on floor 0 as a fallback).
bool cmd_repair(Client *c, const char *cmd, NPC *npcs) {
  if (c->floor_id != 0) {
    send_text_to_client(c->sock,
        "[ERROR] The Blacksmith is only found on floor 0 (city).");
    return true;
  }
  //Check for a merchant/blacksmith near floor 0
  bool near_smith = false;
  FloorIter walk = floor_iter(npcs, 0);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 5) {
      near_smith = true;
      break;
    }
  }
  if (!near_smith) {
    send_text_to_client(c->sock,
        "[ERROR] You must be near the Blacksmith to repair.");
    return true;
  }
  //Collects all repairable slots
  ItemInstance *sls[] = {
      &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
      &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
    &c->slot_arm_l, &c->slot_feet
  };
  //Optional argument: specific slot
  const char *repair_arg = (cmd[6] == ' ') ? cmd + 7 : NULL;
  int total_cost = 0;
  int items_repaired = 0;
  for (int i = 0; i < 10; i++) {
    if (sls[i]->template_idx == -1)
      continue;
    //If an argument is specified, repair only that object
    if (repair_arg != NULL) {
      const char *iname = item_database[sls[i]->template_idx].name;
      if (strcasecmp(iname, repair_arg) != 0 &&
          !(sls[i]->is_artifact &&
            strcasecmp(sls[i]->artifact_name, repair_arg) == 0))
        continue;
    }
    int max_dur = item_database[sls[i]->template_idx].max_durability;
    if (max_dur <= 0)
      continue;
    int diff = max_dur - sls[i]->durability;
    if (diff <= 0)
      continue;
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
    if (sls[i]->quality == QUALITY_RUSTY)
      sls[i]->quality = QUALITY_NORMAL;
    total_cost += item_cost;
    items_repaired++;
    char rep_buf[128];
    get_full_item_name(sls[i], rep_buf, sizeof(rep_buf));
    send_text_to_client(c->sock,
        "[BLACKSMITH] Fixed: %s for %d gp. Durab. now %d/%d.",
        rep_buf, item_cost, sls[i]->durability, max_dur);
  }
  if (items_repaired == 0 && total_cost == 0) {
    send_text_to_client(c->sock,
        "[BLACKSMITH] Your items are in excellent shape, there is nothing"
        "to repair (or you don't have enough gold).");
  } else {
    send_text_to_client(c->sock,
        "[BLACKSMITH] Work completed. Total spent: %d gp.",
        total_cost);
  }
  return true;
}

//uncurse — ask a cleric to remove a curse.
//Cost: 200gp. Requires merchant near floor 0.
bool cmd_uncurse(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  if (c->floor_id != 0) {
    send_text_to_client(c->sock,
        "[ERROR] The Cleric is only on floor 0 (city).");
    return true;
  }
  bool near_cleric = false;
  FloorIter walk = floor_iter(npcs, 0);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
      near_cleric = true;
      break;
    }
  }
  if (!near_cleric) {
    send_text_to_client(c->sock,
        "[ERROR] You must be near the Cleric to remove"
        "to curse.");
    return true;
  }
  if (c->gold < 200) {
    send_text_to_client(c->sock,
        "[CLERIC] The purification rite costs 200gp."
        "You don't have enough gold.");
    return true;
  }
  //Remove the curse from ALL equipped slots
  ItemInstance *sls2[] = {
      &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
      &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
    &c->slot_arm_l, &c->slot_feet
  };
  int uncursed_count = 0;
  for (int i = 0; i < 10; i++) {
    if (sls2[i]->template_idx != -1 &&
        sls2[i]->blessing == BLESS_CURSED) {
      sls2[i]->blessing = BLESS_NORMAL;
      uncursed_count++;
    }
  }
  for (int ri = 0; ri < 10; ri++) {
    if (c->slot_rings[ri].template_idx != -1 &&
        c->slot_rings[ri].blessing == BLESS_CURSED) {
      c->slot_rings[ri].blessing = BLESS_NORMAL;
      uncursed_count++;
    }
  }
  if (uncursed_count == 0) {
    send_text_to_client(c->sock,
        "[CLERIC] Do not carry cursed objects. Save your 200gp.");
    return true;
  }
  c->gold -= 200;
  send_text_to_client(c->sock,
      "[CLERIC] The sacred fire burns the curses! %d item(s)."
      "purified(s) (200gp).", uncursed_count);
  return true;
}

bool cmd_talk(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;)
    if (npcs[i].archetype == ARCH_MERCHANT && abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 2) {
      send_text_to_client(c->sock,
          "[%s] Welcome! Commands: 'list', 'browse <n>', 'buy <n>', 'sell <n>',"
          " 'haggle buy/sell', 'identify [n]', 'repair [name]', 'uncurse', 'cure'.",
          npcs[i].merchant.shop_name);
      print_merchant_inventory(c, &npcs[i]);
      return true;
    }
  return false;
}
