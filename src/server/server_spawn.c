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

#include <math.h>
/**
 * server_spawn.c — Entity spawning module for the Dragon GL server
 *
 * Contains the generation logic for:
 *   - City merchants (floor 0)
 *   - Magic shops (floor 0 and deep floors)
 *   - Dungeon population (floors 1..MAX_FLOORS)
 *   - Ghost/Bones from bones_*.dat files
 *   - Treasures and wandering merchants
 *
 * Global dependencies (defined in main_server.c):
 *   - item_database[], item_database_size  (da data_loader.h)
 *   - bestiary_data[], bestiary_size       (da bestiary.h)
 *   - master_world                         (da server_internal.h)
 */

#include "server_spawn.h"
#include "server_internal.h"
#include "ai.h"
#include "bestiary.h"
#include "data_loader.h"
#include "items.h"
#include "server_entities.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*============================================================================
* Internal (non-public) helper functions
 * ==========================================================================*/

/**
 * add_item_to_shop - Adds an item to an NPC merchant's inventory.
 * @n: Recipient merchant NPC.
 * @template_idx: Index into the item_database of the template to add.
 * @stock: Initial stock quantity.*/
void add_item_to_shop(NPC *n, int template_idx, int stock) {
  if (n->merchant.item_count >= MAX_SHOP_ITEMS) {
    return;
  }
  n->merchant.item_templates[n->merchant.item_count] = template_idx;
  n->merchant.item_stock[n->merchant.item_count] = stock;
  n->merchant.item_stock_max[n->merchant.item_count] = stock;
  n->merchant.item_count++;
}

/**
 * get_item_idx_by_name - Searches for an item in the database by name (case-insensitive).
 * @name: Name of the item to search for.
 *
 * Returns: the index into item_database, or -1 if not found.*/
static int get_item_idx_by_name(const char *name) {
  for (int i = 0; i < item_database_size; i++) {
    if (strcasecmp(item_database[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * is_item_matching_spec - Checks whether an item matches the
 * specialization of a shop.
 * @item_idx: Index into the item_database.
 * @spec: Merchant specialization.
 *
 * Returns: true if the item belongs to the shop category.*/
static bool is_item_matching_spec(int item_idx, MerchantSpecialization spec) {
  ItemTemplate *it = &item_database[item_idx];
  switch (spec) {
  case SHOP_GENERAL:
    return true;
  case BLACKSMITH:
    return (it->category == ITEM_WEAPON || it->category == ITEM_ARMOR ||
            it->category == ITEM_SHIELD || it->category == ITEM_HEAD ||
            it->category == ITEM_HANDS || it->category == ITEM_FEET ||
            it->category == ITEM_AMMO);
  case ALCHEMIST:
    return (it->category == ITEM_CONSUMABLE &&
            (strstr(it->name, "Potion") || strstr(it->name, "Antitoxin"))) ||
           it->category == ITEM_FUEL;
  case MAGIC_SHOP:
    return (it->category == ITEM_RING || it->category == ITEM_NECK ||
            it->category == ITEM_BACK ||
            (it->category == ITEM_CONSUMABLE && it->needs_id &&
             !strstr(it->name, "Ration")));
  case PROVISIONER:
    return (it->category == ITEM_LIGHT_SOURCE || it->category == ITEM_FUEL ||
            it->category == ITEM_MISC ||
            (it->category == ITEM_CONSUMABLE && strstr(it->name, "Ration")));
  case BLACK_MARKET:
    return (it->cost > 5000);
  case SCROLLS:
    return (it->category == ITEM_CONSUMABLE && strstr(it->name, "Scroll"));
  case WANDS_STAFFS:
    return (it->category == ITEM_MISC &&
            (strstr(it->name, "Wand") || strstr(it->name, "Staff")));
  case JEWELRY:
    return (it->category == ITEM_RING || it->category == ITEM_NECK);
  case BOOKS_MAGE:
    return (it->category == ITEM_BOOK &&
            (it->book_class_mask & ((1u << CLASS_WIZARD) |
                                    (1u << CLASS_SORCERER) |
                                    (1u << CLASS_WARLOCK) |
                                    (1u << CLASS_BARD))));
  case BOOKS_PRIEST:
    return (it->category == ITEM_BOOK &&
            (it->book_class_mask & ((1u << CLASS_CLERIC) |
                                    (1u << CLASS_DRUID) |
                                    (1u << CLASS_PALADIN) |
                                    (1u << CLASS_RANGER))));
  case BOOKS_MARTIAL:
    return (it->category == ITEM_BOOK &&
            (it->book_class_mask & ((1u << CLASS_FIGHTER) |
                                    (1u << CLASS_BARBARIAN) |
                                    (1u << CLASS_ROGUE) |
                                    (1u << CLASS_MONK))));
  case CURRENCY_EXCHANGE:
    return (it->category == ITEM_MISC &&
            (strstr(it->name, "Gold") || strstr(it->name, "Silver") ||
             strstr(it->name, "Gem")));
  default:
    return true;
  }
}

/**
 * fill_shop_by_specialization - Fills a merchant's inventory with
 * random items appropriate to his specialization and plan.
 * @n: Merchant NPC to populate.
 * @floor_id: Current floor (used to calibrate the item tier).*/
static void fill_shop_by_specialization(NPC *n, int floor_id) {
  n->merchant.item_count = 0;
  int attempts = 0;
  int target = 50; // Max 50 items

  //Determine the maximum tier based on the plan
  int max_tier = (floor_id / 20) + 1; // Floor 0-19: Tier 1, 20-39: Tier 2…
  if (max_tier > 5) {
    max_tier = 5;
  }

  while (n->merchant.item_count < target && attempts < 5000) {
    int i_idx = rand() % item_database_size;
    if (is_item_matching_spec(i_idx, n->merchant.spec)) {
      //Check cost/tier appropriateness
      uint64_t cost = item_database[i_idx].cost;
      int item_tier = 0;
      if (cost >= 1000000) {
        item_tier = 8;
      } else if (cost >= 500000) {
        item_tier = 7;
      } else if (cost >= 250000) {
        item_tier = 6;
      } else if (cost >= 100000) {
        item_tier = 5;
      } else if (cost >= 20000) {
        item_tier = 4;
      } else if (cost >= 5000) {
        item_tier = 3;
      } else if (cost >= 1000) {
        item_tier = 2;
      } else if (cost >= 100) {
        item_tier = 1;
      } else {
        item_tier = 0;
      }

      if (item_tier >= 6) {
        attempts++;
        continue;
      }

      if (item_tier <= max_tier + 1 || n->merchant.spec == BLACK_MARKET) {
        // Avoid duplicates
        bool dup = false;
        for (int j = 0; j < n->merchant.item_count; j++) {
          if (n->merchant.item_templates[j] == i_idx) {
            dup = true;
          }
        }
        if (!dup) {
          int stock = (item_database[i_idx].category == ITEM_CONSUMABLE)
                          ? (5 + rand() % 10)
                          : (1 + rand() % 3);
          add_item_to_shop(n, i_idx, stock);
        }
      }
    }
    attempts++;
  }
}

/**
 * fill_provisioner_floor0 - Populate a supply store (floor 0) with
 * a curated list of essential survival materials.
 * @n: NPC merchant provider to populate.*/
static void fill_provisioner_floor0(NPC *n) {
  n->merchant.item_count = 0;

  //--- LIGHT SOURCES ---
  int idx;
  idx = get_item_idx_by_name("Torch");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 20);
  }
  idx = get_item_idx_by_name("Lantern, Hooded");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Lantern, Bullseye");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Oil (flask)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 20);
  }

  // --- SURVIVAL AND CAMPING ---
  idx = get_item_idx_by_name("Rations (1 day)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 20);
  }
  idx = get_item_idx_by_name("Waterskin");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 10);
  }
  idx = get_item_idx_by_name("Bedroll");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Rope, Hempen (50 feet)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Rope, Silk (50 feet)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 2);
  }
  idx = get_item_idx_by_name("Tinderbox");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 10);
  }

  // --- TOOLS AND EQUIPMENT ---
  idx = get_item_idx_by_name("Grappling Hook");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Crowbar");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Thieves' Tools");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 2);
  }
  idx = get_item_idx_by_name("Healer's Kit");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Antitoxin (vial)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Acid (vial)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Alchemist's Fire (flask)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }

  // --- CONTAINERS AND TRAVEL ---
  idx = get_item_idx_by_name("Backpack");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Pouch");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Chest");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 2);
  }

  //--- WRITING AND CARTOGRAPHY ---
  idx = get_item_idx_by_name("Ink (1 ounce bottle)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Ink Pen");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Parchment (one sheet)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 20);
  }

  // --- SIMPLE WEAPONS (traveler protection) ---
  idx = get_item_idx_by_name("Dagger");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 5);
  }
  idx = get_item_idx_by_name("Quarterstaff");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Sling");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 3);
  }
  idx = get_item_idx_by_name("Sling Bullets (20)");
  if (idx >= 0) {
    add_item_to_shop(n, idx, 10);
  }

  //If the DB is missing key items, fallback to random filling
  if (n->merchant.item_count < 5) {
    fill_shop_by_specialization(n, 0);
  }
}

/* =========================================================================
 * Public functions
 * ========================================================================= */

void spawn_city_merchants(NPC *npcs, int *next_id) {
  int cx = MAP_CENTER_X;
  int cy = MAP_CENTER_Y;

  // We assign circular coordinates for merchants 0..4.
  // Slot i of the 11-store ring (radius 26), same formula as map.c.
  int coords_x[5], coords_y[5];
  for (int i = 0; i < 5; i++) {
      float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
      coords_x[i] = cx + (int)(cosf(angle) * 26.0f);
      coords_y[i] = cy + (int)(sinf(angle) * 26.0f);
  }

  // 0: Weapons and Armor - The Iron Anvil
  NPC *f = &npcs[0];
  f->active = true;
  f->archetype = ARCH_MERCHANT;
  f->entity_id = (*next_id)++;
  f->floor_id = 0;
  f->x = f->spawn_x = coords_x[0];
  f->y = f->spawn_y = coords_y[0];
  f->respawn_timer = -1;
  f->hp = 1; //hp>0 required: the client drops entities with hp<=0
  f->max_hp = 1;
  f->template = &bestiary_data[0];
  f->merchant.spec = BLACKSMITH;
  strncpy(f->merchant.shop_name, "The Iron Anvil", 63);
  fill_shop_by_specialization(f, 0);

  // 1: Alchemy and Potions - The Bubbling Cauldron
  NPC *p = &npcs[1];
  p->active = true;
  p->archetype = ARCH_MERCHANT;
  p->entity_id = (*next_id)++;
  p->floor_id = 0;
  p->x = p->spawn_x = coords_x[1];
  p->y = p->spawn_y = coords_y[1];
  p->respawn_timer = -1;
  p->hp = 1; //hp>0 required: the client drops entities with hp<=0
  p->max_hp = 1;
  p->template = &bestiary_data[0];
  p->merchant.spec = ALCHEMIST;
  strncpy(p->merchant.shop_name, "The Bubbling Cauldron", 63);
  fill_shop_by_specialization(p, 0);

  // 2: General Store - The Drunken Dragon
  NPC *o = &npcs[2];
  o->active = true;
  o->archetype = ARCH_MERCHANT;
  o->entity_id = (*next_id)++;
  o->floor_id = 0;
  o->x = o->spawn_x = coords_x[2];
  o->y = o->spawn_y = coords_y[2];
  o->respawn_timer = -1;
  o->hp = 1; //hp>0 required: the client drops entities with hp<=0
  o->max_hp = 1;
  o->template = &bestiary_data[0];
  o->merchant.spec = PROVISIONER;
  strncpy(o->merchant.shop_name, "The Drunken Dragon", 63);
  fill_provisioner_floor0(o);

  // 3: Magic and Utility - Temple of Arcana
  NPC *s = &npcs[3];
  s->active = true;
  s->archetype = ARCH_MERCHANT;
  s->entity_id = (*next_id)++;
  s->floor_id = 0;
  s->x = s->spawn_x = coords_x[3];
  s->y = s->spawn_y = coords_y[3];
  s->respawn_timer = -1;
  s->hp = 1; //hp>0 required: the client drops entities with hp<=0
  s->max_hp = 1;
  s->template = &bestiary_data[0];
  s->merchant.spec = MAGIC_SHOP;
  strncpy(s->merchant.shop_name, "Temple of Arcana", 63);
  fill_shop_by_specialization(s, 0);

  // 4: Black Market - The Shadow's Edge
  NPC *m = &npcs[4];
  m->active = true;
  m->archetype = ARCH_MERCHANT;
  m->entity_id = (*next_id)++;
  m->floor_id = 0;
  m->x = m->spawn_x = coords_x[4];
  m->y = m->spawn_y = coords_y[4];
  m->respawn_timer = -1;
  m->hp = 1; //hp>0 required: the client drops entities with hp<=0
  m->max_hp = 1;
  m->template = &bestiary_data[0];
  m->merchant.spec = BLACK_MARKET;
  strncpy(m->merchant.shop_name, "The Shadow's Edge", 63);
  fill_shop_by_specialization(m, 40);
  m->merchant.restock_timer = 100;
}

void spawn_magic_shops(NPC *npcs, int *next_id) {
  int cx = MAP_CENTER_X;
  int cy = MAP_CENTER_Y;

  // We assign circular coordinates for merchants 5..9.
  // Slot i of the 11-store ring (radius 26), same formula as map.c.
  int coords_x[5], coords_y[5];
  for (int i = 5; i < 10; i++) {
      float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
      coords_x[i-5] = cx + (int)(cosf(angle) * 26.0f);
      coords_y[i-5] = cy + (int)(sinf(angle) * 26.0f);
  }

  // 5: Magic and Utility - Master Xanthus
  NPC *m = &npcs[5];
  m->active = true;
  m->archetype = ARCH_MERCHANT;
  m->entity_id = (*next_id)++;
  m->floor_id = 0;
  m->x = m->spawn_x = coords_x[0];
  m->y = m->spawn_y = coords_y[0];
  m->respawn_timer = -1;
  m->hp = 1; //hp>0 required: the client drops entities with hp<=0
  m->max_hp = 1;
  m->template = &bestiary_data[0];
  m->merchant.spec = MAGIC_SHOP;
  strncpy(m->merchant.shop_name, "Xanthus's Arcane Curios", 63);
  fill_shop_by_specialization(m, 20);
  m->merchant.restock_timer = 100;

  //6: Sanctum of Prayers - Sister Elara
  NPC *p = &npcs[6];
  p->active = true;
  p->archetype = ARCH_MERCHANT;
  p->entity_id = (*next_id)++;
  p->floor_id = 0;
  p->x = p->spawn_x = coords_x[1];
  p->y = p->spawn_y = coords_y[1];
  p->respawn_timer = -1;
  p->hp = 1; //hp>0 required: the client drops entities with hp<=0
  p->max_hp = 1;
  p->template = &bestiary_data[0];
  p->merchant.spec = MAGIC_SHOP;
  strncpy(p->merchant.shop_name, "Sister Elara's Sanctum", 63);
  fill_shop_by_specialization(p, 20);
  p->merchant.restock_timer = 100;

  // 7: Catalyst Emporium - Master Malchor
  NPC *c = &npcs[7];
  c->active = true;
  c->archetype = ARCH_MERCHANT;
  c->entity_id = (*next_id)++;
  c->floor_id = 0;
  c->x = c->spawn_x = coords_x[2];
  c->y = c->spawn_y = coords_y[2];
  c->respawn_timer = -1;
  c->hp = 1; //hp>0 required: the client drops entities with hp<=0
  c->max_hp = 1;
  c->template = &bestiary_data[0];
  c->merchant.spec = ALCHEMIST;
  strncpy(c->merchant.shop_name, "Master Malchor's Reagents", 63);
  fill_shop_by_specialization(c, 20);
  c->merchant.restock_timer = 100;

  // 8: Arcane Bookshop (books for mages, warlocks, etc.)
  NPC *r = &npcs[8];
  r->active = true;
  r->archetype = ARCH_MERCHANT;
  r->entity_id = (*next_id)++;
  r->floor_id = 0;
  r->x = r->spawn_x = coords_x[3];
  r->y = r->spawn_y = coords_y[3];
  r->respawn_timer = -1;
  r->hp = 1; //hp>0 required: the client drops entities with hp<=0
  r->max_hp = 1;
  r->template = &bestiary_data[0];
  r->merchant.spec = BOOKS_MAGE;
  strncpy(r->merchant.shop_name, "The Arcane Library", 63);
  fill_shop_by_specialization(r, 100);
  r->merchant.restock_timer = 100;

  // 9: Temple Library (books of clerics, druids, etc.)
  NPC *b = &npcs[9];
  b->active = true;
  b->archetype = ARCH_MERCHANT;
  b->entity_id = (*next_id)++;
  b->floor_id = 0;
  b->x = b->spawn_x = coords_x[4];
  b->y = b->spawn_y = coords_y[4];
  b->respawn_timer = -1;
  b->hp = 1; //hp>0 required: the client drops entities with hp<=0
  b->max_hp = 1;
  b->template = &bestiary_data[0];
  b->merchant.spec = BOOKS_PRIEST;
  strncpy(b->merchant.shop_name, "Temple Library", 63);
  fill_shop_by_specialization(b, 100);
  b->merchant.restock_timer = 100;
}

/* =========================================================================
 * spawn_martial_archive - Spawns the martial-arts bookshop on floor 0.
 * Sells the discipline codices of Fighter, Barbarian, Rogue and Monk.
 * Building: slot 10 of the 11-store ring (radius 26) in map.c,
 * built by the same generic shop loop as the other ten stores.
 * Occupies NPC slot 10.
 * ========================================================================= */
void spawn_martial_archive(NPC *npcs, int *next_id) {
  int cx = MAP_CENTER_X;
  int cy = MAP_CENTER_Y;
  // Slot 10 of the 11-store ring (radius 26), same formula as map.c
  float angle = (10 * (360.0f / 11.0f)) * (M_PI / 180.0f);
  int ax = cx + (int)(cosf(angle) * 26.0f);
  int ay = cy + (int)(sinf(angle) * 26.0f);

  NPC *a = &npcs[10];
  a->active = true;
  a->archetype = ARCH_MERCHANT;
  a->entity_id = (*next_id)++;
  a->floor_id = 0;
  a->x = a->spawn_x = ax;
  a->y = a->spawn_y = ay;
  a->respawn_timer = -1;
  a->hp = 1; //hp>0 required: the client drops entities with hp<=0
  a->max_hp = 1;
  a->template = &bestiary_data[0];
  a->merchant.spec = BOOKS_MARTIAL;
  strncpy(a->merchant.shop_name, "The Archive of a Thousand Battles", 63);
  fill_shop_by_specialization(a, 100);
  a->merchant.restock_timer = 100;
}

void populate_dungeons(NPC *npcs, int *next_id) {
  int npc_idx = 11; // The 11 city merchants occupy slots 0-10



  //--- POPULATION OF FLOORS ---
  for (int f = 1; f < MAX_FLOORS; f++) {

    // --- GHOST/BONES SYSTEM ---
    char fname[128];
    snprintf(fname, sizeof(fname), "data/bones_%d.dat", f);
    FILE *bfile = fopen(fname, "rb");
    if (bfile) {
      BonesData b;
      if (fread(&b, sizeof(BonesData), 1, bfile) == 1) {
        if (npc_idx < MAX_NPCS) {
          NPC *g = &npcs[npc_idx++];
          g->active = true;
          g->archetype = ARCH_BOSS;
          g->entity_id = (*next_id)++;
          g->floor_id = f;
          g->x = b.x;
          g->y = b.y;
          g->spawn_x = b.x;
          g->spawn_y = b.y;
          g->template = &bestiary_data[0];
          //Look for a Ghost/Wraith/Spirit template in the bestiary
          for (int i = 0; i < bestiary_size; i++) {
            if (strcasestr(bestiary_data[i].name, "Ghost") ||
                strcasestr(bestiary_data[i].name, "Wraith") ||
                strcasestr(bestiary_data[i].name, "Spirit")) {
              g->template = &bestiary_data[i];
              break;
            }
          }
          g->hp = 250;
          g->max_hp = 250;
          g->is_ghost = true;
          strncpy(g->custom_name, b.name, sizeof(g->custom_name) - 1);
          for (int i = 0; i < 30; i++) {
            g->ghost_loot[i].template_idx = b.items[i];
            g->ghost_loot[i].stack_count = b.amounts[i];
          }
          ai_init_npc(g, g->template->name, g->floor_id);
        }
      }
      fclose(bfile);
      remove(fname); //Removed: Spawns only once
    }


    //--- MONSTER SELECTION filtered by floor_min/floor_max ---
    //Builds a pool of monsters suited to this floor
    int *floor_pool = NULL;
    int pool_size = 0;
    for (int bi = 0; bi < bestiary_size; bi++) {
      if (f >= bestiary_data[bi].floor_min && f <= bestiary_data[bi].floor_max) {
        pool_size++;
      }
    }
    if (pool_size > 0) {
      floor_pool = malloc(pool_size * sizeof(int));
      int pi = 0;
      for (int bi = 0; bi < bestiary_size; bi++) {
        if (f >= bestiary_data[bi].floor_min && f <= bestiary_data[bi].floor_max) {
          floor_pool[pi++] = bi;
        }
      }
    }

    //--- BOSS FLOOR (every 10 floors) ---
    if (f > 0 && f % 10 == 0) {
      if (pool_size > 0 && npc_idx < MAX_NPCS) {
        int r_idx = rand() % pool_size;
        int boss_id = floor_pool[r_idx];

        NPC *b = &npcs[npc_idx++];
        b->active = true;
        b->archetype = ARCH_BOSS;
        b->entity_id = (*next_id)++;
        b->floor_id = f;
        b->x = MAP_CENTER_X;
        b->y = MAP_CENTER_Y;
        b->spawn_x = MAP_CENTER_X;
        b->spawn_y = MAP_CENTER_Y;
        b->respawn_timer = 0;
        b->template = &bestiary_data[boss_id];
        
        //Appropriate sizing of the boss
        b->hp = b->template->hp_avg * 5 + (f * 50);
        b->max_hp = b->hp;
        
        //Let's name it "Boss"
        snprintf(b->custom_name, sizeof(b->custom_name), "Boss %s", b->template->name);
        
        ai_init_npc(b, b->custom_name, b->floor_id);
      }
      if (floor_pool) free(floor_pool);
      continue; //Boss floors only have the boss
    }


    //--- SPAWN MONSTERS on the floor ---
    //Map 300x300 = 90000 tiles. Fewer monsters to avoid exceeding MAX_NPCS
    int num_monsters = 100 + (f * 4); 
    if (num_monsters > 300) {
      num_monsters = 300;
    }

    for (int m = 0; m < num_monsters; m++) {
      if (npc_idx >= MAX_NPCS) {
        break;
      }

      //Find a valid VOXEL_FLOOR tile
      int px = -1;
      int py = -1;
      for (int tries = 0; tries < 100; tries++) {
        int tx = rand() % MAP_WIDTH;
        int ty = rand() % MAP_HEIGHT;
        VoxelType v = master_world->floors[f].map.data[0][ty][tx];
        if (v == VOXEL_FLOOR || v == VOXEL_WOOD || v == VOXEL_SAND || 
            v == VOXEL_MUD || v == VOXEL_ICE || v == VOXEL_GRASS || 
            v == VOXEL_COBBLE || v == VOXEL_MARBLE || v == VOXEL_ASH) {
          px = tx;
          py = ty;
          break;
        }
      }

      if (px != -1) {
        int t_id;
        if (floor_pool && pool_size > 0) {
          t_id = floor_pool[rand() % pool_size];
        } else {
          t_id = rand() % bestiary_size;
        }

        NPC *n = &npcs[npc_idx++];
        n->active = true;
        n->archetype = ARCH_MELEE; //ai_init refines it
        n->entity_id = (*next_id)++;
        n->floor_id = f;
        n->x = px;
        n->y = py;
        n->spawn_x = px;
        n->spawn_y = py;
        n->respawn_timer = 0;
        n->template_idx = t_id;
        n->template = &bestiary_data[t_id];
        n->effect_count = 0;
        //ai_init_npc calculates hp, max_hp, ac, attack_bonus, damage dice
        //and assign archetype + behavior tree based on the monster name
        ai_init_npc(n, n->template->name, n->floor_id);
      }
    }

    if (floor_pool) {
      free(floor_pool);
    }

    //--- LOOT on the floor: Heaps of Gold and Chests ---
    int num_treasures = 15 + (f * 2);
    for (int c_idx = 0; c_idx < num_treasures; c_idx++) {
      if (npc_idx >= MAX_NPCS) {
        break;
      }
      int lx = -1;
      int ly = -1;
      for (int tries = 0; tries < 100; tries++) {
        int tx = rand() % MAP_WIDTH;
        int ty = rand() % MAP_HEIGHT;
        VoxelType v = master_world->floors[f].map.data[0][ty][tx];
        if (v == VOXEL_FLOOR || v == VOXEL_WOOD || v == VOXEL_SAND || 
            v == VOXEL_MUD || v == VOXEL_ICE || v == VOXEL_GRASS || 
            v == VOXEL_COBBLE || v == VOXEL_MARBLE || v == VOXEL_ASH) {
          lx = tx;
          ly = ty;
          break;
        }
      }
      if (lx != -1) {
        NPC *s = &npcs[npc_idx++];
        s->active = true;
        s->entity_id = (*next_id)++;
        s->floor_id = f;
        s->x = lx;
        s->y = ly;
        s->spawn_x = lx;
        s->spawn_y = ly;
        s->respawn_timer = 0;
        s->template_idx = -1;
        s->template = NULL; //No template, so it doesn't respawn as a monster
        s->hp = 9999;
        s->max_hp = 9999;

        if (rand() % 4 == 0) {
          s->archetype = ARCH_TREASURE;
          s->gold_drop = 0; //Items are generated upon pickup
        } else {
          s->archetype = ARCH_GOLD;
          s->gold_drop = 50 + (rand() % (100 * f)); // Scales with depth
        }
      }
    }

    //--- WANDERING MERCHANT every 5 floors ---
    if (f % 5 == 0 && npc_idx < MAX_NPCS) {
      int lx = -1;
      int ly = -1;
      for (int tries = 0; tries < 100; tries++) {
        int tx = rand() % MAP_WIDTH;
        int ty = rand() % MAP_HEIGHT;
        if (master_world->floors[f].map.data[0][ty][tx] == VOXEL_FLOOR) {
          lx = tx;
          ly = ty;
          break;
        }
      }
      if (lx != -1) {
        NPC *s = &npcs[npc_idx++];
        s->active = true;
        s->archetype = ARCH_MERCHANT;
        s->entity_id = (*next_id)++;
        s->floor_id = f;
        s->x = lx;
        s->y = ly;
        s->spawn_x = lx;
        s->spawn_y = ly;
        s->respawn_timer = -1;
        s->template_idx = 0;
        s->template = &bestiary_data[0];
        s->hp = 1;
        s->max_hp = 1;
        s->merchant.restock_timer = 100;

        //Asset-based variety: picks from almost all available specializations
        int roll = rand() % 8;
        switch (roll) {
        case 0:
          s->merchant.spec = BLACKSMITH;
          strncpy(s->merchant.shop_name, "Errant Blacksmith", 63);
          break;
        case 1:
          s->merchant.spec = ALCHEMIST;
          strncpy(s->merchant.shop_name, "Hidden Alchemist", 63);
          break;
        case 2:
          s->merchant.spec = SCROLLS;
          strncpy(s->merchant.shop_name, "Scroll Peddler", 63);
          break;
        case 3:
          s->merchant.spec = WANDS_STAFFS;
          strncpy(s->merchant.shop_name, "Wand Weaver", 63);
          break;
        case 4:
          s->merchant.spec = JEWELRY;
          strncpy(s->merchant.shop_name, "Glimmering Trader", 63);
          break;
        case 5:
          s->merchant.spec = MAGIC_SHOP;
          strncpy(s->merchant.shop_name, "Arcane Smuggler", 63);
          break;
        case 6:
          s->merchant.spec = PROVISIONER;
          strncpy(s->merchant.shop_name, "Walking Provisioner", 63);
          break;
        case 7:
          s->merchant.spec = BLACK_MARKET;
          strncpy(s->merchant.shop_name, "Shadow Dealer", 63);
          break;
        }
        fill_shop_by_specialization(s, f);
      }
    }
  }
}
