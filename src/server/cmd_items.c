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

#include "server_cmd.h"
#include "server_internal.h"
#include "server_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

bool cmd_retrieve(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
    int ghost_npc_idx = -1;
    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "Ghost of %s", c->username);

    FloorIter walk = floor_iter(npcs, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (npcs[i].is_ghost && abs(npcs[i].x - c->x) <= 1 && abs(npcs[i].y - c->y) <= 1) {
            if (strcmp(npcs[i].custom_name, expected_name) == 0) {
                ghost_npc_idx = i;
                break;
            }
        }
    }

    if (ghost_npc_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] No ghosts of you nearby.");
        return true;
    }

    if (npcs[ghost_npc_idx].gold_drop > 0) {
        c->gold += npcs[ghost_npc_idx].gold_drop;
        npcs[ghost_npc_idx].gold_drop = 0;
    }
    for (int gi = 0; gi < 30; gi++) {
        if (npcs[ghost_npc_idx].ghost_loot[gi].template_idx >= 0 &&
            npcs[ghost_npc_idx].ghost_loot[gi].stack_count > 0 && c->backpack_count < MAX_BACKPACK) {
            c->backpack[c->backpack_count++] = npcs[ghost_npc_idx].ghost_loot[gi];
            npcs[ghost_npc_idx].ghost_loot[gi].template_idx = -1;
            npcs[ghost_npc_idx].ghost_loot[gi].stack_count  = 0;
        }
    }

    send_text_to_client(c->sock, "[SYSTEM] You have recovered your soul and equipment!");
    npc_set_active(&npcs[ghost_npc_idx], false);
    npcs[ghost_npc_idx].respawn_timer = 0;
    master_world->floors[c->floor_id].entity_grid[npcs[ghost_npc_idx].y][npcs[ghost_npc_idx].x] = 0;
    save_player_data(c);
    return true;
}

bool cmd_inventory(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  float w = get_current_weight(c);
  send_text_to_client(
      c->sock,
      "================ INVENTORY (%.1f/%.1f kg) ================", w,
      (float)(c->str * 5));

  ItemInstance *sl[] = {&c->slot_head,  &c->slot_neck,   &c->slot_body,
                        &c->slot_back,  &c->slot_hand_r, &c->slot_hand_l,
                        &c->slot_hands, &c->slot_arm_r, &c->slot_arm_l,
                        &c->slot_feet};
  const char *ns[] = {"HEAD", "NECK", "BODY", "BACK",
                      "R.HAND",  "L.HAND",  "HANDS",  "R.ARM", "L.ARM", "FEET"};

  send_text_to_client(c->sock, "--- EQUIPPED ---");
  for (int i = 0; i < 10; i++) {
    if (sl[i]->template_idx != -1) {
      char buf[128];
      get_full_item_name(sl[i], buf, sizeof(buf));
      send_text_to_client(c->sock, " [%-7s] %s (Dur:%d/%d)", ns[i],
                          buf,
                          sl[i]->durability,
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
      send_text_to_client(
          c->sock, " [RING %d] %s", i + 1, buf);
    }
  }

  send_text_to_client(c->sock, "--- ON BELT ---");
  for (int i = 0; i < MAX_BELT; i++) {
    if (c->belt[i].template_idx != -1) {
      char buf[128];
      get_full_item_name(&c->belt[i], buf, sizeof(buf));
      send_text_to_client(c->sock, " [SLOT %d] %s (x%d)", i + 1,
                          buf,
                          c->belt[i].stack_count);
    }
  }

  send_text_to_client(c->sock, "--- IN BACKPACK ---");
  if (c->backpack_count == 0) {
    send_text_to_client(c->sock, "  (empty backpack)");
  } else {
    for (int i = 0; i < c->backpack_count; i++) {
      char buf[128];
      get_full_item_name(&c->backpack[i], buf, sizeof(buf));
      send_text_to_client(
          c->sock, " [%d] %s (x%d)", i + 1,
          buf,
          c->backpack[i].stack_count);
    }
  }
  send_text_to_client(
      c->sock, "==========================================================");
  return true;
}

bool cmd_wear(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = strchr(cmd, ' '); if (input) input++; else return true;
  ItemInstance *t = NULL;
  int loc = -1, si = -1;

  //1. Search for the item in your backpack or belt (by name or index)
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count) {
      t = &c->backpack[requested_idx];
      loc = 0;
      si = requested_idx;
    }
  } else {
    //Search your backpack by name
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1 &&
          strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
        t = &c->backpack[i];
        loc = 0;
        si = i;
        break;
      }
    }
    //Search by name in belt
    if (!t) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 &&
            strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
          t = &c->belt[i];
          loc = 1;
          si = i;
          break;
        }
      }
    }
  }

  if (!t) {
    send_text_to_client(c->sock, "[ERROR] Object not found (use name"
                                 "or backpack number).");
    return true;
  }

  const ItemTemplate *it = &item_database[t->template_idx];
  ItemInstance *slot = NULL;
  switch (it->category) {
  case ITEM_WEAPON:
  case ITEM_SHIELD:
    if (c->slot_hand_r.template_idx == -1)
      slot = &c->slot_hand_r;
    else
      slot = &c->slot_hand_l;
    break;
  case ITEM_HEAD:
    slot = &c->slot_head;
    break;
  case ITEM_NECK:
    slot = &c->slot_neck;
    break;
  case ITEM_ARMOR:
    slot = &c->slot_body;
    break;
  case ITEM_BACK:
    slot = &c->slot_back;
    break;
  case ITEM_HANDS:
    slot = &c->slot_hands;
    break;
  case ITEM_FEET:
    slot = &c->slot_feet;
    break;
  case ITEM_BRACELET:
    if (c->slot_arm_r.template_idx == -1)
      slot = &c->slot_arm_r;
    else
      slot = &c->slot_arm_l;
    break;
  case ITEM_RING:
    for (int r = 0; r < 10; r++) {
      if (c->slot_rings[r].template_idx == -1) {
        slot = &c->slot_rings[r];
        break;
      }
    }
    if (!slot)
      slot = &c->slot_rings[0]; //Overwrites the first one if full
    break;
  default:
    send_text_to_client(c->sock,
                        "[ERROR] You cannot wear this item.");
    return true;
  }

  if (slot) {
    ItemInstance prev = *slot;
    *slot = *t;
    // Remove from source
    if (loc == 0) {
      c->backpack[si] = c->backpack[c->backpack_count - 1];
      c->backpack_count--;
    } else {
      c->belt[si].template_idx = -1;
    }
    //Place the removed item in your backpack
    if (prev.template_idx != -1) {
      if (c->backpack_count < MAX_BACKPACK) {
        c->backpack[c->backpack_count++] = prev;
      } else {
        send_text_to_client(c->sock, "[WARNING] Full backpack, the object"
                                     "removed it was lost in the void!");
      }
    }
    //===== BLIND IDENTIFICATION (Mechanics 2) =====
    //Wearing an unidentified object reveals it... but if it's Cursed
    //it's a trap!
    if (!slot->is_identified) {
      slot->is_identified = true;
      char id_buf[128];
      get_full_item_name(slot, id_buf, sizeof(id_buf));
      if (slot->blessing == BLESS_CURSED) {
        send_text_to_client(c->sock,
            "[MAGIC] By wearing the object you identify it: it's %s!",
            id_buf);
        send_text_to_client(c->sock,
            "[CURSE] A dark aura envelops your hand!"
            "The item is CURSED and you can't take it away!");
      } else {
        send_text_to_client(c->sock,
            "[MAGIC] By wearing the object you identify it: it's %s!",
            id_buf);
      }
    } else {
      send_text_to_client(c->sock, "[SYSTEM] You have equipped %s.", it->name);
    }
    send_detailed_state(c);
  }
  return true;
}

//===== MECHANICAL 1: REMOVE (remove equipped item) =====
/*"t <dir>" is tunnel, so this command has no one-letter alias.*/
bool cmd_remove(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = strchr(cmd, ' '); if (input) input++; else return true;
  //Slots map by name
  ItemInstance *sls[] = {
      &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
      &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
    &c->slot_arm_l, &c->slot_feet
  };
  const char *snames[] = {
      "head", "neck", "body", "back",
      "hand_r", "hand_l", "hands", "arm_r", "arm_l", "feet"
  };
  ItemInstance *target_slot = NULL;
  int scount = 10;
  //Search by slot name or item name
  for (int si2 = 0; si2 < scount; si2++) {
    if (sls[si2]->template_idx == -1)
      continue;
    const char *iname = item_database[sls[si2]->template_idx].name;
    bool slot_match = (strcasecmp(input, snames[si2]) == 0);
    if (!slot_match) {
      if (si2 == 7 && (strcasecmp(input, "right_arm") == 0 || strcasecmp(input, "arm_r") == 0 || strcasecmp(input, "r.a") == 0)) slot_match = true;
      else if (si2 == 8 && (strcasecmp(input, "left_arm") == 0 || strcasecmp(input, "arm_l") == 0 || strcasecmp(input, "l.a") == 0)) slot_match = true;
      else if ((si2 == 7 || si2 == 8) && (strcasecmp(input, "arm") == 0 || strcasecmp(input, "arms") == 0)) slot_match = true;
    }
    if (strcasestr(iname, input) != NULL || slot_match ||
        (sls[si2]->is_artifact &&
         strcasestr(sls[si2]->artifact_name, input) != NULL)) {
      target_slot = sls[si2];
      break;
    }
  }
  //Also check the rings
  if (!target_slot) {
    for (int ri = 0; ri < 10; ri++) {
      if (c->slot_rings[ri].template_idx == -1)
        continue;
      const char *iname = item_database[c->slot_rings[ri].template_idx].name;
      if (strcasestr(iname, input) != NULL) {
        target_slot = &c->slot_rings[ri];
        break;
      }
    }
  }
  //Also check your seat belt
  if (!target_slot) {
    for (int bi = 0; bi < MAX_BELT; bi++) {
      if (c->belt[bi].template_idx == -1)
        continue;
      const char *iname = item_database[c->belt[bi].template_idx].name;
      if (strcasestr(iname, input) != NULL) {
        target_slot = &c->belt[bi];
        break;
      }
    }
  }
  if (!target_slot) {
    send_text_to_client(c->sock,
        "[ERROR] Item not found among those equipped."
        "Use 'stats' to see what you're wearing.");
    return true;
  }
  //===== CURSE BLOCK (Heart of Mechanics 1) =====
  if (target_slot->blessing == BLESS_CURSED && target_slot->is_identified) {
    char curse_buf[128];
    get_full_item_name(target_slot, curse_buf, sizeof(curse_buf));
    send_text_to_client(c->sock,
        "[CURSE] The object '%s' is surrounded by an evil aura!"
        "You can't take it off!",
        curse_buf);
    send_text_to_client(c->sock,
        "[TIP] Find a cleric and use 'uncurse' (200gp)"
        "or use a Scroll of Curse Removal.");
    return true;
  }
  //Normal removal: put in backpack
  if (c->backpack_count >= MAX_BACKPACK) {
    send_text_to_client(c->sock,
        "[ERROR] Backpack full! Empty your backpack first.");
    return true;
  }
  char rem_buf[128];
  get_full_item_name(target_slot, rem_buf, sizeof(rem_buf));
  c->backpack[c->backpack_count++] = *target_slot;
  target_slot->template_idx = -1;
  send_text_to_client(c->sock,
      "[SYSTEM] You have removed %s and moved it to the backpack.", rem_buf);
  send_detailed_state(c);
  return true;
}

bool cmd_belt(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = cmd + 5;
  int bi = -1;
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count)
      bi = requested_idx;
  } else {
    for (int i = 0; i < c->backpack_count; i++)
      if (strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
        bi = i;
        break;
      }
  }

  if (bi != -1) {
    for (int i = 0; i < MAX_BELT; i++) {
      if (c->belt[i].template_idx == -1) {
        c->belt[i] = c->backpack[bi];
        c->backpack[bi] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
        send_text_to_client(c->sock, "[SYSTEM] %s moved to belt.",
                            item_database[c->belt[i].template_idx].name);
        send_detailed_state(c); //Update client view radius
        return true;
      }
    }
    send_text_to_client(c->sock, "[ERROR] Belt full!");
  } else {
    send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
  }
  return true;
}

bool cmd_unbelt(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = strchr(cmd, ' ');
  if (input) input++;
  int slot_idx = -1;
  if (input && isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < MAX_BELT && c->belt[requested_idx].template_idx != -1) {
      slot_idx = requested_idx;
    }
  } else if (input) {
    for (int i = 0; i < MAX_BELT; i++) {
      if (c->belt[i].template_idx != -1 &&
          strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
        slot_idx = i;
        break;
      }
    }
  } else {
    /*If unbelt without arguments, move the first belted item into the backpack*/
    for (int i = 0; i < MAX_BELT; i++) {
      if (c->belt[i].template_idx != -1) {
        slot_idx = i;
        break;
      }
    }
  }

  if (slot_idx != -1) {
    if (c->backpack_count >= MAX_BACKPACK) {
      send_text_to_client(c->sock, "[ERROR] Backpack full!");
      return true;
    }
    ItemInstance item = c->belt[slot_idx];
    c->backpack[c->backpack_count++] = item;
    c->belt[slot_idx].template_idx = -1;
    c->belt[slot_idx].stack_count = 0;
    send_text_to_client(c->sock, "[SYSTEM] %s moved from belt to backpack.",
                        item_database[item.template_idx].name);
    send_detailed_state(c);
  } else {
    send_text_to_client(c->sock, "[ERROR] No items found in belt (use 'unbelt <n>' or 'unbelt <name>').");
  }
  return true;
}

// lore <name> — inspects an item in the backpack or reads its content if it is a book
bool cmd_lore(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = strchr(cmd, ' ');
  if (input) {
    input++;
  } else {
    send_text_to_client(c->sock, "[ERROR] Specify the object to inspect (e.g. 'lore 1' or 'lore book').");
    return true;
  }
  int found_idx = -1;
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
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
        send_text_to_client(c->sock, "  [#%2d] %-25s (Req. Lv PG: %d)",
                            global_idx, tmpl->book_spell_names[local_idx], required_pg_level);
      }
    } else {
      send_text_to_client(c->sock, "(No readable spells listed here)");
    }
    send_text_to_client(c->sock, "--------------------------------");
  }
  return true;
}

//fill <n> - fills a lantern using an oil (fuel) from the backpack
bool cmd_fill(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *input = cmd + 5;
  int lantern_idx = -1;
  bool in_belt = false;
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
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
    send_text_to_client(c->sock, "[ERROR] Item to fill not found (search by name or number in backpack).");
    return true;
  }

  ItemInstance *target = in_belt ? &c->belt[lantern_idx] : &c->backpack[lantern_idx];
  ItemTemplate *ltmpl = &item_database[target->template_idx];

  if (ltmpl->category != ITEM_LIGHT_SOURCE) {
    send_text_to_client(c->sock, "[ERROR] '%s' is not a rechargeable light source.", ltmpl->name);
    return true;
  }

  if (strcasestr(ltmpl->name, "torch") || strcasestr(ltmpl->name, "Torcia")) {
    send_text_to_client(c->sock, "[ERROR] The torches just wear out, they cannot be refilled with oil.");
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
    send_text_to_client(c->sock, "[ERROR] You don't have any oil or fuel in your backpack to refill.");
    return true;
  }

  ItemInstance *fuel = &c->backpack[fuel_idx];
  ItemTemplate *ftmpl = &item_database[fuel->template_idx];

  if (target->durability >= ltmpl->max_durability) {
    send_text_to_client(c->sock, "[OBJECT] %s is already full of fuel.", ltmpl->name);
    return true;
  }

  target->durability = ltmpl->max_durability;
  send_text_to_client(c->sock, "[OBJECT] You filled %s using a dose of %s.", ltmpl->name, ftmpl->name);

  fuel->stack_count--;
  if (fuel->stack_count <= 0) {
    c->backpack[fuel_idx] = c->backpack[c->backpack_count - 1];
    c->backpack_count--;
  }
  return true;
}

/*Learns the spells of a book: only with the ritual command of the class,
 * inside its temple on floor 0, and only the spells within the level.*/
static void learn_from_book(Client *c, int found_idx, ItemTemplate *tmpl, bool is_ritual_cmd) {
  if (!is_ritual_cmd) {
    send_text_to_client(c->sock, "[ERROR] Use 'study' (or your class ritual) to learn from this book, or use 'lore' to browse it.");
    return;
  }
  /* =======================================================
   * RITUAL LEARNING SYSTEM
   *
   * To learn spells from a book the player MUST:
   *   1. Be physically inside the temple of their
   *      class (on floor 0).
   *   2. Have a sufficient player level for each
   *      individual spell in the book.
   *   3. Use the ritual command of their class
   *      (study / pray / awaken / invoke / intone /
   *       celebrate / pronounce / practice).
   *
   * The book is NEVER consumed: it must be kept
   * in the inventory. If lost or sold, the spells
   * remain known, but the book will need to be repurchased.
   * ======================================================= */

  /* ---- Temple definitions by class (floor 0) ----
   * Each temple is a rectangle [x0,x1] x [y0,y1].
   * Coordinates centered at cx/cy=500; see map.c.
   *
   *  Wizard   : X[494..506] Y[439..451]  (North, BLUE crystal)
   *  Paladin  : X[532..544] Y[455..467]  (North-East, YELLOW crystal)
   *  Cleric   : X[549..561] Y[494..506]  (East, YELLOW crystal)
   *  Sorcerer : X[532..544] Y[532..544]  (South-East, RED crystal)
   *  Warlock  : X[494..506] Y[549..561]  (South, GREEN crystal)
   *  Bard     : X[455..467] Y[532..544]  (South-West, RED crystal)
   *  Druid    : X[439..451] Y[494..506]  (West, GREEN crystal)
   *  Ranger   : X[455..467] Y[455..467]  (North-West, GREEN crystal)
   */

  /* Temple table is now global */

  /*---- 1. Check class compatible with the book ----*/
  uint32_t cls_bit = (1u << (int)c->class_id);
  if (!(tmpl->book_class_mask & cls_bit)) {
    send_text_to_client(c->sock,
        "[BOOK] '%s' is written in a language that your class"
        "(%s) does not include.",
        tmpl->name, CLASSES[c->class_id].name);
    return;
  }

  /*---- 2. Locate the PC's class temple ----*/
  const TempleInfo *my_temple = NULL;
  for (int ti = 0; ti < TEMPLE_COUNT; ti++) {
    if (TEMPLES[ti].cls == (int)c->class_id) {
      my_temple = &TEMPLES[ti];
      break;
    }
  }

  if (!my_temple) {
    /*Non-magical class (e.g. Fighter, Rogue, Barbarian, Monk)*/
    send_text_to_client(c->sock,
        "[BOOK] Class %s does not have a learning temple:"
        "you cannot activate this book.", CLASSES[c->class_id].name);
    return;
  }

  /*---- 3. Check location: the PC must be in the temple ----*/
  bool in_temple = (
    c->floor_id == 0 &&
    c->x >= my_temple->x0 && c->x <= my_temple->x1 &&
    c->y >= my_temple->y0 && c->y <= my_temple->y1
  );

  if (!in_temple) {
    send_text_to_client(c->sock,
        "[RITUAL] To activate '%s' you must go to floor 0, in %s,"
        "and use the command '%s %d' while you are physically inside.",
        tmpl->name, my_temple->name,
        my_temple->ritual, found_idx + 1);
    return;
  }

  /*---- 4. Determine the PC's level (1..20) ----*/
  /*We use the character level saved in the client state*/
  int pg_level = c->level;
  if (pg_level < 1) {
    pg_level = 1;
  }

  /*---- 5. Learns spells from the ordered list in the book ----
   * Level gate:
   * global_idx = book_seq * 10 + local_location
   * required_level = global_idx / 3
   * (60 total spells per class / 20 PC levels = 3 spells unlocked per level)*/
  int learned = 0;
  int locked  = 0;
  int already = 0;

  send_text_to_client(c->sock,
      "[RITUAL] %s... '%s' (you are in %s, Lv PG %d):",
      my_temple->ritual, tmpl->name, my_temple->name, pg_level);

  int use_ordered = (tmpl->book_spell_count > 0);

  if (use_ordered) {
    /*--- New path: explicit spell list in book ---*/
    for (int local_idx = 0; local_idx < tmpl->book_spell_count; local_idx++) {
      const char *spell_name = tmpl->book_spell_names[local_idx];

      /*Find the spell in the database*/
      int si = -1;
      for (int k = 0; k < spell_database_size; k++) {
        if (strcasecmp(spell_database[k].name, spell_name) == 0) {
          si = k;
          break;
        }
      }
      if (si < 0) {
        continue;
      }

      SpellTemplate *sp = &spell_database[si];

      /*Check that the spell is accessible to the PC's class*/
      if (!(sp->class_mask & cls_bit)) {
        continue;
      }

      /*New adjusted DragonGL curve for cap at level 20: Required level = (Magic Level * 2)*/
      int required_pg_level = (sp->level > 0) ? (sp->level * 2) : 1;

      if (pg_level < required_pg_level) {
        send_text_to_client(c->sock,
            "  [-] %-30s  (requires PC Lv %d — LOCKED)",
            sp->name, required_pg_level);
        locked++;
        continue;
      }

      /*Check if already known (guarded: si >= MAX_SPELL_DB_SIZE
       * must never index past the known_spells bitfield)*/
      bool already_known = spell_known_get(c, si);

      if (already_known) {
        send_text_to_client(c->sock,
            "  [Lv%d] %-30s  (already in grimoire)",
            sp->level, sp->name);
        already++;
      } else {
        /*Learn magic*/
        spell_known_set(c, si);
        send_text_to_client(c->sock,
            "  [Lv%d] %-30s  *** LEARNED ***",
            sp->level, sp->name);
        learned++;
      }
    }
  } else {
    /*--- A book without a spell list: its level range decides ---*/
    for (int si = 0; si < spell_database_size; si++) {
      SpellTemplate *sp = &spell_database[si];

      /*The magic must be in the range of the book*/
      if (sp->level < tmpl->book_min_level || sp->level > tmpl->book_max_level) {
        continue;
      }

      /*Magic must be accessible to the PC's class*/
      if (!(sp->class_mask & cls_bit)) {
        continue;
      }

      /*New DragonGL curves (no copyright 5e)*/
      int required_pg_level = (sp->level > 0) ? (sp->level * 2 + (sp->level / 2)) : 1;
      if (pg_level < required_pg_level) {
        send_text_to_client(c->sock,
            "  [Lv%d] %-30s  (requires PC Lv %d - LOCKED)",
            sp->level, sp->name, required_pg_level);
        locked++;
        continue;
      }

      /*Check if already known (guarded, see above)*/
      bool already_known = spell_known_get(c, si);

      if (already_known) {
        send_text_to_client(c->sock,
            "  [Lv%d] %-30s  (already in grimoire)",
            sp->level, sp->name);
        already++;
      } else {
        /*Learn magic*/
        spell_known_set(c, si);
        send_text_to_client(c->sock,
            "  [Lv%d] %-30s  *** LEARNED ***",
            sp->level, sp->name);
        learned++;
      }
    }
  }

  /*---- 6. Summary message ----*/
  if (learned == 0 && already == 0 && locked == 0) {
    send_text_to_client(c->sock,
        "[RITUAL] No compatible spells found for your class.");
  } else {
    if (learned > 0) {
      send_text_to_client(c->sock,
          "[RITUAL] You have learned %d new magic(s)! Use 'spells' for"
          "see your updated library.", learned);
    }
    if (locked > 0) {
      send_text_to_client(c->sock,
          "[RITUAL] %d spell(s) are still blocked by your level."
          "Keep the book and return when you are more powerful.", locked);
    }
    if (learned == 0 && locked == 0) {
      send_text_to_client(c->sock,
          "[RITUAL] You already know all the accessible magic in this book.");
    }
  }

  /*The book is NOT removed from inventory*/
  c->needs_study = false;
}

//use <name> — uses a consumable from your backpack or belt
//The ritual commands (study/pray/awaken/invoke/intone/celebrate/pronounce/practice)
//they are aliases for 'read' but can only be activated within the temple of your class.
bool cmd_use(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  bool is_eat_cmd = (strcmp(cmd, "eat") == 0 || strncmp(cmd, "eat ", 4) == 0 || strcmp(cmd, "e") == 0 || strncmp(cmd, "e ", 2) == 0);
  const char *input = strchr(cmd, ' ');
  if (input) {
    input++;
  } else if (is_eat_cmd) {
    /*Auto-search for food/rations in backpack if 'eat' is typed without arguments*/
    input = "ration";
  } else {
    send_text_to_client(c->sock, "[ERROR] Specify the subject (e.g. 'use 1' or 'study book').");
    return true;
  }
  int found_idx = -1;
  bool in_belt = false;

  bool is_ritual_cmd = (strncmp(cmd, "study", 5) == 0 ||
                        strncmp(cmd, "pray", 4) == 0 ||
                        strncmp(cmd, "awaken", 6) == 0 ||
                        strncmp(cmd, "invoke", 6) == 0 ||
                        strncmp(cmd, "intone", 6) == 0 ||
                        strncmp(cmd, "celebrate", 9) == 0 ||
                        strncmp(cmd, "pronounce", 9) == 0 ||
                        strncmp(cmd, "practice", 8) == 0 ||
                        strncmp(cmd, "drill", 5) == 0 ||
                        strncmp(cmd, "rage", 4) == 0 ||
                        strncmp(cmd, "sneak", 5) == 0 ||
                        strncmp(cmd, "meditate", 8) == 0);

  //1. Search the backpack by index
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count) {
      found_idx = requested_idx;
    }
  } else {
    //2. Search your backpack by name
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1 &&
          strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
        found_idx = i;
        break;
      }
    }
    //3. Search by name in belt
    if (found_idx == -1) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
            strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
          found_idx = i;
          in_belt = true;
          break;
        }
      }
    }
  }

  //If found on the belt (by name), let's momentarily move it to the backpack
  //for the consumption process
  if (found_idx != -1 && in_belt) {
    if (c->backpack_count < MAX_BACKPACK) {
      c->backpack[c->backpack_count] = c->belt[found_idx];
      c->belt[found_idx].template_idx = -1;
      found_idx = c->backpack_count;
      c->backpack_count++;
    } else {
      send_text_to_client(c->sock, "[ERROR] Backpack too full for"
                                   "handle the belt item!");
      return true;
    }
  }

  if (found_idx == -1) {
    send_text_to_client(c->sock,
                        "[ERROR] Item not found in inventory (use"
                        "name or number).");
    return true;
  }

  ItemTemplate *tmpl = &item_database[c->backpack[found_idx].template_idx];
  if (!c->backpack[found_idx].is_identified) {
    send_text_to_client(c->sock,
                        "[ERROR] You must first identify this object.");
    return true;
  }

  if (is_ritual_cmd && tmpl->category != ITEM_BOOK) {
    send_text_to_client(c->sock, "[ERROR] Ritual commands (like 'study') are only for learning from magical books.");
    return true;
  }

  if (tmpl->category == ITEM_BOOK) {
    learn_from_book(c, found_idx, tmpl, is_ritual_cmd);
    return true;
  }

  if (tmpl->category != ITEM_CONSUMABLE && tmpl->category != ITEM_FUEL) {
    send_text_to_client(
        c->sock,
        "[ERROR] %s is not consumable or usable as fuel.",
        tmpl->name);
    return true;
  }

  //Case: Fuel (Oil) for Lanterns
  if (tmpl->category == ITEM_FUEL) {
    bool refilled = false;
    ItemInstance *ls[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                          &c->belt[1],     &c->belt[2],     &c->belt[3]};
    for (int j = 0; j < 6; j++) {
      if (ls[j]->template_idx != -1) {
        const ItemTemplate *lit = &item_database[ls[j]->template_idx];
        //If the object is a lantern (light radius > 6)
        if (lit->category == ITEM_LIGHT_SOURCE && lit->light_radius > 6) {
          ls[j]->durability = lit->max_durability;
          refilled = true;
          send_text_to_client(c->sock,
                              "[SYSTEM] You have replenished your %s with %s.",
                              lit->name, tmpl->name);
          send_detailed_state(c); //Update client view radius
          break;
        }
      }
    }
    if (!refilled) {
      send_text_to_client(c->sock, "[ERROR] You don't have a lantern ready from"
                                   "Replenish in hand or on belt.");
      return true;
    }
  } else if (tmpl->heal_amount > 0) {
    //Care effect
    int healed = tmpl->heal_amount;
    c->hp += healed;
    if (c->hp > c->max_hp)
      c->hp = c->max_hp;
    if (strcasestr(tmpl->name, "Ration") != NULL) {
      c->hunger_level = 0;
      send_text_to_client(c->sock,
                          "[ITEM] You ate %s. Hunger fully satisfied! (+%d HP).",
                          tmpl->name, healed);
    } else {
      unsigned int hash = 5381;
      for (int i = 0; tmpl->name[i] != '\0'; i++) hash = ((hash << 5) + hash) + tmpl->name[i];
      float col_r = 0.2f + (float)(hash % 80) / 100.0f;
      float col_g = 0.2f + (float)((hash >> 8) % 80) / 100.0f;
      float col_b = 0.2f + (float)((hash >> 16) % 80) / 100.0f;
      float m_col = (col_r > col_g) ? ((col_r > col_b) ? col_r : col_b) : ((col_g > col_b) ? col_g : col_b);
      if (m_col < 1.0f) { col_r /= m_col; col_g /= m_col; col_b /= m_col; }
      broadcast_spell_vfx(c->x, c->y, c->x, c->y, 2, col_r, col_g, col_b, c->floor_id);

      send_text_to_client(c->sock,
                          "[ITEM] You used %s and recovered %d HP (HP: %d/%d).",
                          tmpl->name, healed, c->hp, c->max_hp);
    }
  } else {
    //Unhealed Item: Apply a generic effect
    unsigned int hash = 5381;
    for (int i = 0; tmpl->name[i] != '\0'; i++) hash = ((hash << 5) + hash) + tmpl->name[i];
    float col_r = 0.2f + (float)(hash % 80) / 100.0f;
    float col_g = 0.2f + (float)((hash >> 8) % 80) / 100.0f;
    float col_b = 0.2f + (float)((hash >> 16) % 80) / 100.0f;
    float m_col = (col_r > col_g) ? ((col_r > col_b) ? col_r : col_b) : ((col_g > col_b) ? col_g : col_b);
    if (m_col < 1.0f) { col_r /= m_col; col_g /= m_col; col_b /= m_col; }
    broadcast_spell_vfx(c->x, c->y, c->x, c->y, 2, col_r, col_g, col_b, c->floor_id);

    if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
      ActiveEffect ef;
      ef.name = tmpl->name;
      ef.trigger = EVENT_ON_TURN_START;
      ef.mod_type = MOD_ADDITIVE;
      ef.value = 2;
      ef.duration_rounds =
          tmpl->duration_turns > 0 ? tmpl->duration_turns : 10;
      ef.is_persistent = false;
      c->effects[c->effect_count++] = ef;
      send_text_to_client(c->sock,
                          "[ITEM] You used %s. Effect active for %d rounds.",
                          tmpl->name, ef.duration_rounds);
    } else {
      send_text_to_client(c->sock, "[ITEM] You used %s, but your magic-saturated body dispels the effect.", tmpl->name);
    }
  }
  //Remove a unit from your backpack
  c->backpack[found_idx].stack_count--;
  if (c->backpack[found_idx].stack_count <= 0) {
    c->backpack[found_idx] = c->backpack[c->backpack_count - 1];
    c->backpack_count--;
  }
  send_detailed_state(c); //Instantly sync HP, inventory and Vitality (hunger) with the HUD client
  return true;
}

// --- RECOVERY FROM TOMBSTONE ---
bool cmd_pickup(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  tombstone_pickup(c);
  save_player_data(c);
  return true;
}

// --- MISSING ROGUELIKE COMMANDS ---
bool cmd_get(Client *c, const char *cmd, NPC *npcs) {

  int target_x = c->x;
  int target_y = c->y;
  int target_idx = -1;

  char dir[16] = {0};
  char idx_str[16] = {0};
  const char *args = cmd + (cmd[0] == 'g' && cmd[1] == 'e' ? 3 : 1);
  while (*args == ' ') args++;

  if (sscanf(args, "%15s %15s", dir, idx_str) >= 1) {
     if (isdigit(dir[0])) {
         target_idx = atoi(dir);
     } else {
         if (strcasecmp(dir, "N") == 0) target_y--;
         else if (strcasecmp(dir, "S") == 0) target_y++;
         else if (strcasecmp(dir, "W") == 0) target_x--;
         else if (strcasecmp(dir, "E") == 0) target_x++;
         else if (strcasecmp(dir, "NW") == 0) { target_y--; target_x--; }
         else if (strcasecmp(dir, "NE") == 0) { target_y--; target_x++; }
         else if (strcasecmp(dir, "SW") == 0) { target_y++; target_x--; }
         else if (strcasecmp(dir, "SE") == 0) { target_y++; target_x++; }

         if (idx_str[0] && isdigit(idx_str[0])) {
             target_idx = atoi(idx_str);
         }
     }
  }

  int items[64];
  int count = 0;
  FloorIter walk = floor_iter(npcs, c->floor_id);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
      if (npcs[i].x == target_x && npcs[i].y == target_y) {
          if (npcs[i].archetype == ARCH_TREASURE || npcs[i].archetype == ARCH_GOLD) {
              if (count < 64) items[count++] = i;
          }
      }
  }

  if (count == 0) {
      send_text_to_client(c->sock, "[SYSTEM] There is nothing to collect here.");
      return true;
  }

  if (count == 1 || target_idx > 0) {
      int pick = (target_idx > 0 && target_idx <= count) ? items[target_idx - 1] : items[0];
      NPC *n = &npcs[pick];
      if (n->archetype == ARCH_GOLD) {
          c->gold += n->gold_drop;
          send_text_to_client(c->sock, "[SYSTEM] You have collected %d gold coins!", n->gold_drop);
          send_combat_event(c->sock, COMBAT_EV_GOLD, c->entity_id, n->gold_drop);
          npc_set_active(n, false);
      } else if (n->archetype == ARCH_TREASURE) {
          if (n->ghost_loot[0].stack_count > 0) {
              if (c->backpack_count >= MAX_BACKPACK) {
                  send_text_to_client(c->sock, "[SYSTEM] Backpack full! Unable to collect.");
              } else {
                  c->backpack[c->backpack_count++] = n->ghost_loot[0];
                  send_text_to_client(c->sock, "[SYSTEM] You have collected: %s", item_database[n->ghost_loot[0].template_idx].name);
                  npc_set_active(n, false);
                  n->respawn_timer = 0;
              }
          } else {
              send_text_to_client(c->sock, "[SYSTEM] Open the chest and find something...");
              drop_loot_from_monster(c, n);
              npc_set_active(n, false);
              n->respawn_timer = 100;
          }
      }
      /*The floor counters follow npc_set_active() at each removal.*/
      save_player_data(c);
  } else {
      send_text_to_client(c->sock, "[SYSTEM] There are more objects:");
      for (int i = 0; i < count; i++) {
          NPC *n = &npcs[items[i]];
          if (n->archetype == ARCH_GOLD) {
              char buf[128];
              snprintf(buf, sizeof(buf), "%d. %d gold coins", i + 1, n->gold_drop);
              send_text_to_client(c->sock, buf);
          } else if (n->ghost_loot[0].stack_count > 0) {
              char buf[128];
              snprintf(buf, sizeof(buf), "  %d. %s", i + 1, item_database[n->ghost_loot[0].template_idx].name);
              send_text_to_client(c->sock, buf);
          } else {
              char buf[128];
              snprintf(buf, sizeof(buf), "%d. Treasure chest", i + 1);
              send_text_to_client(c->sock, buf);
          }
      }
      char prompt[128];
      if (target_x == c->x && target_y == c->y) {
          snprintf(prompt, sizeof(prompt), "What do you want to collect? Use 'get <n>'.");
      } else {
          snprintf(prompt, sizeof(prompt), "What do you want to collect? Use 'get %s <n>'.", dir);
      }
      send_text_to_client(c->sock, prompt);
  }
  return true;
}

bool cmd_drop(Client *c, const char *cmd, NPC *npcs) {
  const char *input = strchr(cmd, ' ');
  if (input) {
    input++;
  } else {
    return true;
  }
  int found_idx = -1;
  bool in_belt = false;

  //1. Search the backpack by index
  if (isdigit(input[0])) {
    int requested_idx = atoi(input) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count) {
      found_idx = requested_idx;
    }
  } else {
    //2. Search your backpack by name
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1 &&
          strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
        found_idx = i;
        break;
      }
    }
    //3. Search by name in belt
    if (found_idx == -1) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
            strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
          found_idx = i;
          in_belt = true;
          break;
        }
      }
    }
  }

  if (found_idx == -1) {
    send_text_to_client(c->sock, "[SYSTEM] Object '%s' not found.", input);
    return true;
  }

  ItemInstance *item_to_drop = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];

  //Find empty NPC slots
  int npc_slot = -1;
  for (int i = 0; i < MAX_NPCS; i++) {
    if (!npcs[i].active) {
      npc_slot = i;
      break;
    }
  }

  if (npc_slot == -1) {
    send_text_to_client(c->sock, "[SYSTEM] Unable to drop item: too many items on the ground.");
    return true;
  }

  // Assign NPC as dropped item (activation happens AFTER the fields
  // below are final — see npc_set_active)
  npcs[npc_slot].entity_id = next_id++;
  npcs[npc_slot].floor_id = c->floor_id;
  npcs[npc_slot].x = c->x;
  npcs[npc_slot].y = c->y;
  npcs[npc_slot].archetype = ARCH_TREASURE;
  npcs[npc_slot].template_idx = item_to_drop->template_idx; // Used for rendering client-side
  npcs[npc_slot].hp = npcs[npc_slot].max_hp = 1;
  memset(npcs[npc_slot].ghost_loot, 0, sizeof(npcs[npc_slot].ghost_loot));
  npcs[npc_slot].ghost_loot[0] = *item_to_drop; // Copy full item state
  npcs[npc_slot].template = NULL;
  /*template==NULL: the floor counters ignore the slot, but the
   * activation still goes through npc_set_active (floor index).*/
  npc_set_active(&npcs[npc_slot], true);

  // Register on the grid so players can pick it up
  master_world->floors[c->floor_id].entity_grid[c->y][c->x] = npcs[npc_slot].entity_id;

  send_text_to_client(c->sock, "[ACTION] You dropped: %s on the ground", item_database[item_to_drop->template_idx].name);

  // Remove from inventory
  if (in_belt) {
    c->belt[found_idx].template_idx = -1;
    c->belt[found_idx].stack_count = 0;
  } else {
    for (int i = found_idx; i < c->backpack_count - 1; i++) {
      c->backpack[i] = c->backpack[i + 1];
    }
    c->backpack_count--;
  }
  return true;
}

bool cmd_throw(Client *c, const char *cmd, NPC *npcs) {
  const char *input = strchr(cmd, ' ');
  if (input) {
    input++;
  } else {
    return true;
  }
  char item_query[64] = {0};
  char dir_char = '\0';

  const char *last_space = strrchr(input, ' ');
  if (last_space) {
      dir_char = last_space[1];
      int len = last_space - input;
      if (len > 63) len = 63;
      strncpy(item_query, input, len);
      item_query[len] = '\0';
  } else {
      send_text_to_client(c->sock, "[SYSTEM] Specifies the direction. Example: throw dart n");
      return true;
  }

  if (dir_char != 'n' && dir_char != 's' && dir_char != 'e' && dir_char != 'w') {
      send_text_to_client(c->sock, "[SYSTEM] Invalid direction. Use: n, s, e, w (e.g. throw dart e)");
      return true;
  }

  int found_idx = -1;
  bool in_belt = false;

  if (item_query[0] >= '0' && item_query[0] <= '9') {
    int requested_idx = atoi(item_query) - 1;
    if (requested_idx >= 0 && requested_idx < c->backpack_count) {
      found_idx = requested_idx;
    }
  } else {
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1 &&
          strcasestr(item_database[c->backpack[i].template_idx].name, item_query) != NULL) {
        found_idx = i;
        break;
      }
    }
    if (found_idx == -1) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
            strcasestr(item_database[c->belt[i].template_idx].name, item_query) != NULL) {
          found_idx = i;
          in_belt = true;
          break;
        }
      }
    }
  }

  if (found_idx == -1) {
    send_text_to_client(c->sock, "[SYSTEM] Item '%s' not found in your inventory.", item_query);
    return true;
  }

  ItemInstance *item_to_throw = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];
  ItemTemplate *t_item = &item_database[item_to_throw->template_idx];

  int dx = 0, dy = 0;
  if (dir_char == 'n') dy = -1;
  else if (dir_char == 's') dy = 1;
  else if (dir_char == 'e') dx = 1;
  else if (dir_char == 'w') dx = -1;

  int max_range = 5;
  if (t_item->category == ITEM_AMMO) max_range = 8;

  int curr_x = c->x;
  int curr_y = c->y;
  int last_valid_x = c->x;
  int last_valid_y = c->y;
  int hit_npc_idx = -1;

  for (int step = 1; step <= max_range; step++) {
    int next_x = curr_x + dx;
    int next_y = curr_y + dy;

    if (next_x < 0 || next_x >= MAP_WIDTH || next_y < 0 || next_y >= MAP_HEIGHT) {
      break;
    }

    uint8_t voxel = master_world->floors[c->floor_id].map.data[0][next_y][next_x];
    if (voxel == 0 || voxel == 2 || voxel == 3) {
      break;
    }

    curr_x = next_x;
    curr_y = next_y;
    last_valid_x = curr_x;
    last_valid_y = curr_y;

    FloorIter walk = floor_iter(npcs, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
      if (npcs[i].x == curr_x && npcs[i].y == curr_y && npcs[i].archetype != ARCH_TREASURE && npcs[i].archetype != ARCH_GOLD) {
        hit_npc_idx = i;
        break;
      }
    }

    if (hit_npc_idx != -1) {
      break;
    }
  }

  send_text_to_client(c->sock, "[ACTION] You throw %s towards %c...", t_item->name, dir_char);

  broadcast_spell_vfx(c->x, c->y, last_valid_x, last_valid_y, 0, 1.0f, 0.8f, 0.3f, c->floor_id);

  if (hit_npc_idx != -1) {
    NPC *target = &npcs[hit_npc_idx];
    int roll = (rand() % 20) + 1;
    int to_hit = roll + rules_get_modifier(c->dex) + t_item->attack_bonus;
    if (to_hit >= target->ac) {
      int dice = t_item->damage_dice_count > 0 ? t_item->damage_dice_count : 1;
      int sides = t_item->damage_dice_sides > 0 ? t_item->damage_dice_sides : 4;
      int dmg = rules_roll_dice(dice, sides);
      target->hp -= dmg;

      send_text_to_client(c->sock, "[COMBAT] Hit %s for %d damage! (HP: %d/%d)",
                          target->template ? target->template->name : "the target", dmg, target->hp, target->max_hp);

      if (target->hp <= 0) {
        npc_set_active(target, false);
        target->respawn_timer = RESPAWN_TICKS;
        int gained_xp = target->template ? target->template->xp : 10;
        c->xp += gained_xp;
        check_level_up(c);
        send_text_to_client(c->sock, "[VICTORY] You defeated %s by throwing %s at them! (+%d XP)", target->template ? target->template->name : "the target", t_item->name, gained_xp);
        if (target->archetype == ARCH_BOSS) {
          handle_boss_death(c, target);
        }
      }
    } else {
      send_text_to_client(c->sock, "[COMBAT] Missed! %s dodges the throw.", target->template ? target->template->name : "the target");
    }
  }

  int npc_slot = -1;
  for (int i = 0; i < MAX_NPCS; i++) {
    if (!npcs[i].active) {
      npc_slot = i;
      break;
    }
  }

  if (npc_slot != -1) {
    /*Fields first, activation last (npc_set_active)*/
    npcs[npc_slot].entity_id = next_id++;
    npcs[npc_slot].floor_id = c->floor_id;
    npcs[npc_slot].x = last_valid_x;
    npcs[npc_slot].y = last_valid_y;
    npcs[npc_slot].archetype = ARCH_TREASURE;
    npcs[npc_slot].template_idx = item_to_throw->template_idx;
    npcs[npc_slot].hp = npcs[npc_slot].max_hp = 1;
    memset(npcs[npc_slot].ghost_loot, 0, sizeof(npcs[npc_slot].ghost_loot));
    npcs[npc_slot].ghost_loot[0] = *item_to_throw;
    npcs[npc_slot].template = NULL;
    npc_set_active(&npcs[npc_slot], true);

    master_world->floors[c->floor_id].entity_grid[last_valid_y][last_valid_x] = npcs[npc_slot].entity_id;
  }

  if (in_belt) {
    c->belt[found_idx].template_idx = -1;
    c->belt[found_idx].stack_count = 0;
  } else {
    for (int i = found_idx; i < c->backpack_count - 1; i++) {
      c->backpack[i] = c->backpack[i + 1];
    }
    c->backpack_count--;
  }

  return true;
}
