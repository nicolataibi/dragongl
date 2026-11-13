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
 *   - Treasures and wandering merchants
 */

#include "server_spawn.h"
#include "server_internal.h"
#include "server_world.h"
#include "ai.h"
#include "bestiary.h"
#include "data_loader.h"
#include "items.h"
#include "server_entities.h"
#include "server_tiles.h"

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
                (it->category == ITEM_CONSUMABLE && it->needs_id && !strstr(it->name, "Ration")));
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
                (it->book_class_mask & ((1u << CLASS_WIZARD) | (1u << CLASS_SORCERER) |
                                        (1u << CLASS_WARLOCK) | (1u << CLASS_BARD))));
    case BOOKS_PRIEST:
        return (it->category == ITEM_BOOK &&
                (it->book_class_mask & ((1u << CLASS_CLERIC) | (1u << CLASS_DRUID) |
                                        (1u << CLASS_PALADIN) | (1u << CLASS_RANGER))));
    case BOOKS_MARTIAL:
        return (it->category == ITEM_BOOK &&
                (it->book_class_mask & ((1u << CLASS_FIGHTER) | (1u << CLASS_BARBARIAN) |
                                        (1u << CLASS_ROGUE) | (1u << CLASS_MONK))));
    case CURRENCY_EXCHANGE:
        return (
            it->category == ITEM_MISC &&
            (strstr(it->name, "Gold") || strstr(it->name, "Silver") || strstr(it->name, "Gem")));
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
    /*Light, survival and camping, tools, containers, writing, and a few
     * simple weapons for the traveler.*/
    static const struct {
        const char *item;
        int stock;
    } k_stock[] = {
        {"Torch", 20},
        {"Lantern, Hooded", 5},
        {"Lantern, Bullseye", 3},
        {"Oil (flask)", 20},
        {"Rations (1 day)", 20},
        {"Waterskin", 10},
        {"Bedroll", 5},
        {"Rope, Hempen (50 feet)", 5},
        {"Rope, Silk (50 feet)", 2},
        {"Tinderbox", 10},
        {"Grappling Hook", 3},
        {"Crowbar", 3},
        {"Thieves' Tools", 2},
        {"Healer's Kit", 5},
        {"Antitoxin (vial)", 5},
        {"Acid (vial)", 3},
        {"Alchemist's Fire (flask)", 3},
        {"Backpack", 5},
        {"Pouch", 5},
        {"Chest", 2},
        {"Ink (1 ounce bottle)", 5},
        {"Ink Pen", 5},
        {"Parchment (one sheet)", 20},
        {"Dagger", 5},
        {"Quarterstaff", 3},
        {"Sling", 3},
        {"Sling Bullets (20)", 10},
    };
    n->merchant.item_count = 0;
    for (size_t i = 0; i < sizeof(k_stock) / sizeof(k_stock[0]); i++) {
        int idx = get_item_idx_by_name(k_stock[i].item);
        if (idx >= 0) {
            add_item_to_shop(n, idx, k_stock[i].stock);
        }
    }
    //If the DB is missing key items, fallback to random filling
    if (n->merchant.item_count < 5) {
        fill_shop_by_specialization(n, 0);
    }
}

/* =========================================================================
 * Public functions
 * ========================================================================= */

void spawn_city_merchants(Game *game) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;

    // We assign circular coordinates for merchants 0..4.
    // Slot i of the 11-store ring (radius 26), same formula as map.c.
    int coords_x[5], coords_y[5];
    for (int i = 0; i < 5; i++) {
        float angle = city_shop_angle(i);
        coords_x[i] = cx + (int)(cosf(angle) * 26.0f);
        coords_y[i] = cy + (int)(sinf(angle) * 26.0f);
    }

    // 0: Weapons and Armor - The Iron Anvil
    NPC *f = &game->npcs[0];
    npc_set_active(game, f, true); /*merchant: no floor-cache effect*/
    f->archetype = ARCH_MERCHANT;
    f->entity_id = game->next_id++;
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
    NPC *p = &game->npcs[1];
    npc_set_active(game, p, true); /*merchant: no floor-cache effect*/
    p->archetype = ARCH_MERCHANT;
    p->entity_id = game->next_id++;
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
    NPC *o = &game->npcs[2];
    npc_set_active(game, o, true); /*merchant: no floor-cache effect*/
    o->archetype = ARCH_MERCHANT;
    o->entity_id = game->next_id++;
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
    NPC *s = &game->npcs[3];
    npc_set_active(game, s, true); /*merchant: no floor-cache effect*/
    s->archetype = ARCH_MERCHANT;
    s->entity_id = game->next_id++;
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
    NPC *m = &game->npcs[4];
    npc_set_active(game, m, true); /*merchant: no floor-cache effect*/
    m->archetype = ARCH_MERCHANT;
    m->entity_id = game->next_id++;
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

void spawn_magic_shops(Game *game) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;

    // We assign circular coordinates for merchants 5..9.
    // Slot i of the 11-store ring (radius 26), same formula as map.c.
    int coords_x[5], coords_y[5];
    for (int i = 5; i < 10; i++) {
        float angle = city_shop_angle(i);
        coords_x[i - 5] = cx + (int)(cosf(angle) * 26.0f);
        coords_y[i - 5] = cy + (int)(sinf(angle) * 26.0f);
    }

    // 5: Magic and Utility - Master Xanthus
    NPC *m = &game->npcs[5];
    npc_set_active(game, m, true); /*merchant: no floor-cache effect*/
    m->archetype = ARCH_MERCHANT;
    m->entity_id = game->next_id++;
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
    NPC *p = &game->npcs[6];
    npc_set_active(game, p, true); /*merchant: no floor-cache effect*/
    p->archetype = ARCH_MERCHANT;
    p->entity_id = game->next_id++;
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
    NPC *c = &game->npcs[7];
    npc_set_active(game, c, true); /*merchant: no floor-cache effect*/
    c->archetype = ARCH_MERCHANT;
    c->entity_id = game->next_id++;
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
    NPC *r = &game->npcs[8];
    npc_set_active(game, r, true); /*merchant: no floor-cache effect*/
    r->archetype = ARCH_MERCHANT;
    r->entity_id = game->next_id++;
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
    NPC *b = &game->npcs[9];
    npc_set_active(game, b, true); /*merchant: no floor-cache effect*/
    b->archetype = ARCH_MERCHANT;
    b->entity_id = game->next_id++;
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
void spawn_martial_archive(Game *game) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;
    // Slot 10 of the 11-store ring (radius 26), same formula as map.c
    float angle = city_shop_angle(10);
    int ax = cx + (int)(cosf(angle) * 26.0f);
    int ay = cy + (int)(sinf(angle) * 26.0f);

    NPC *a = &game->npcs[10];
    npc_set_active(game, a, true); /*merchant: no floor-cache effect*/
    a->archetype = ARCH_MERCHANT;
    a->entity_id = game->next_id++;
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

/*A random tile a creature can stand on (any walkable ground, or stone
 * floor only), in 100 tries; false if none was found.*/
static bool random_spawn_tile(Game *game, int f, bool floor_only, int *x, int *y) {
    for (int tries = 0; tries < 100; tries++) {
        int tx = rand() % MAP_WIDTH;
        int ty = rand() % MAP_HEIGHT;
        VoxelType v = game->world->floors[f].map.data[0][ty][tx];
        bool ok = v == VOXEL_FLOOR;
        if (!floor_only) {
            ok = ok || v == VOXEL_WOOD || v == VOXEL_SAND || v == VOXEL_MUD || v == VOXEL_ICE ||
                 v == VOXEL_GRASS || v == VOXEL_COBBLE || v == VOXEL_MARBLE || v == VOXEL_ASH;
        }
        if (ok) {
            *x = tx;
            *y = ty;
            return true;
        }
    }
    return false;
}

/*The bestiary entries whose floor range holds f; the caller frees the
 * array (NULL when *size is 0).*/
static int *floor_monster_pool(int f, int *size) {
    int *pool = malloc((size_t)(bestiary_size > 0 ? bestiary_size : 1) * sizeof(int));
    *size = 0;
    for (int bi = 0; pool && bi < bestiary_size; bi++) {
        if (f >= bestiary_data[bi].floor_min && f <= bestiary_data[bi].floor_max) {
            pool[(*size)++] = bi;
        }
    }
    if (*size == 0) {
        free(pool);
        return NULL;
    }
    return pool;
}

/*Fields shared by every new NPC of the dungeon; activation comes last,
 * by the caller, once template and archetype are final.*/
static NPC *dungeon_npc(Game *game, int *npc_idx, int f, int x, int y) {
    NPC *n = &game->npcs[(*npc_idx)++];
    n->entity_id = game->next_id++;
    n->floor_id = f;
    n->x = n->spawn_x = x;
    n->y = n->spawn_y = y;
    n->respawn_timer = 0;
    return n;
}

/*The boss of a boss floor: a monster of the floor's pool with five times
 * its hit points and 50 more per floor, in the middle of the map.*/
static void spawn_boss(Game *game, int *npc_idx, int f, const int *pool, int size) {
    if (size <= 0 || *npc_idx >= MAX_NPCS) {
        return;
    }
    int boss_id = pool[rand() % size];
    int bx, by;
    find_walkable_near(&game->world->floors[f], MAP_CENTER_X, MAP_CENTER_Y, &bx, &by);
    NPC *b = dungeon_npc(game, npc_idx, f, bx, by);
    b->archetype = ARCH_BOSS;
    b->template = &bestiary_data[boss_id];
    b->hp = b->template->hp_avg * 5 + (f * 50);
    b->max_hp = b->hp;
    snprintf(b->custom_name, sizeof(b->custom_name), "Boss %s", b->template->name);
    ai_init_npc(b, b->custom_name, b->floor_id);
    npc_set_active(game, b, true);
}

/*100 monsters, 4 more per floor (at most 300), of the floor's pool.*/
static void spawn_monsters(Game *game, int *npc_idx, int f, const int *pool, int size) {
    int num_monsters = 100 + (f * 4);
    if (num_monsters > 300) {
        num_monsters = 300;
    }
    for (int m = 0; m < num_monsters && *npc_idx < MAX_NPCS; m++) {
        int px, py;
        if (!random_spawn_tile(game, f, false, &px, &py)) {
            continue;
        }
        int t_id = size > 0 ? pool[rand() % size] : rand() % bestiary_size;
        NPC *n = dungeon_npc(game, npc_idx, f, px, py);
        n->archetype = ARCH_MELEE; /*ai_init_npc refines it*/
        n->template_idx = t_id;
        n->template = &bestiary_data[t_id];
        n->effect_count = 0;
        n->morale = 0;
        /*hp, AC, attack, damage dice, archetype and behavior from the template*/
        ai_init_npc(n, n->template->name, n->floor_id);
        npc_set_active(game, n, true);
    }
}

/*15 piles, 2 more per floor: one in four is a chest (its item is rolled
 * when it is opened), the others gold that scales with the depth.*/
static void spawn_treasures(Game *game, int *npc_idx, int f) {
    int num_treasures = 15 + (f * 2);
    for (int i = 0; i < num_treasures && *npc_idx < MAX_NPCS; i++) {
        int lx, ly;
        if (!random_spawn_tile(game, f, false, &lx, &ly)) {
            continue;
        }
        NPC *s = dungeon_npc(game, npc_idx, f, lx, ly);
        s->template_idx = -1;
        s->template = NULL; /*no template: it never respawns as a monster*/
        s->hp = 9999;
        s->max_hp = 9999;
        if (rand() % 4 == 0) {
            s->archetype = ARCH_TREASURE;
            s->gold_drop = 0;
        } else {
            s->archetype = ARCH_GOLD;
            s->gold_drop = 50 + (rand() % (100 * f));
        }
        npc_set_active(game, s, true);
    }
}

/*A wandering merchant of a random trade, on a stone floor tile.*/
static void spawn_wandering_merchant(Game *game, int *npc_idx, int f) {
    static const struct {
        MerchantSpecialization spec;
        const char *shop_name;
    } k_trades[8] = {
        {BLACKSMITH, "Errant Blacksmith"},    {ALCHEMIST, "Hidden Alchemist"},
        {SCROLLS, "Scroll Peddler"},          {WANDS_STAFFS, "Wand Weaver"},
        {JEWELRY, "Glimmering Trader"},       {MAGIC_SHOP, "Arcane Smuggler"},
        {PROVISIONER, "Walking Provisioner"}, {BLACK_MARKET, "Shadow Dealer"},
    };
    int lx, ly;
    if (*npc_idx >= MAX_NPCS || !random_spawn_tile(game, f, true, &lx, &ly)) {
        return;
    }
    NPC *s = dungeon_npc(game, npc_idx, f, lx, ly);
    s->archetype = ARCH_MERCHANT;
    s->respawn_timer = -1;
    s->template_idx = 0;
    s->template = &bestiary_data[0];
    s->hp = 1;
    s->max_hp = 1;
    s->merchant.restock_timer = 100;
    int trade = rand() % 8;
    s->merchant.spec = k_trades[trade].spec;
    copy_str(s->merchant.shop_name, k_trades[trade].shop_name, sizeof(s->merchant.shop_name));
    npc_set_active(game, s, true); /*merchant: no floor-cache effect*/
    fill_shop_by_specialization(s, f);
}

/*Populates every dungeon floor of a new world: a boss alone on every
 * tenth floor; elsewhere monsters, gold and chests, and a wandering
 * merchant on every fifth floor.*/
void populate_dungeons(Game *game) {
    int npc_idx = 11; // The 11 city merchants occupy slots 0-10
    for (int f = 1; f < MAX_FLOORS; f++) {
        int pool_size = 0;
        int *floor_pool = floor_monster_pool(f, &pool_size);
        if (f % 10 == 0) {
            spawn_boss(game, &npc_idx, f, floor_pool, pool_size);
        } else {
            spawn_monsters(game, &npc_idx, f, floor_pool, pool_size);
            spawn_treasures(game, &npc_idx, f);
            if (f % 5 == 0) {
                spawn_wandering_merchant(game, &npc_idx, f);
            }
        }
        free(floor_pool);
    }
}
