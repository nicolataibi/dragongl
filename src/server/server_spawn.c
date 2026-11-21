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
void add_item_to_shop(MerchantData *m, int template_idx, int stock) {
    if (m->item_count >= MAX_SHOP_ITEMS) {
        return;
    }
    m->item_templates[m->item_count] = template_idx;
    m->item_stock[m->item_count] = stock;
    m->item_stock_max[m->item_count] = stock;
    m->item_count++;
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
/*A staff or a rod held as a weapon, or a magic robe worn as clothing: the
 * wands-and-staffs shop and the magic shop sell them, not the smith.*/
static bool is_staff(const ItemTemplate *it) {
    return it->category == ITEM_WEAPON &&
           (strncmp(it->name, "Staff of", 8) == 0 || strncmp(it->name, "Rod of", 6) == 0);
}

static bool is_robe(const ItemTemplate *it) {
    return it->category == ITEM_ARMOR && strncmp(it->name, "Robe", 4) == 0;
}

static bool is_item_matching_spec(int item_idx, MerchantSpecialization spec) {
    ItemTemplate *it = &item_database[item_idx];
    switch (spec) {
    case SHOP_GENERAL:
        return true;
    case BLACKSMITH:
        return (it->category == ITEM_WEAPON || it->category == ITEM_ARMOR ||
                it->category == ITEM_SHIELD || it->category == ITEM_HEAD ||
                it->category == ITEM_HANDS || it->category == ITEM_FEET ||
                it->category == ITEM_AMMO) &&
               !is_staff(it) && !is_robe(it);
    case ALCHEMIST:
        return (it->category == ITEM_CONSUMABLE &&
                (it->is_potion || strstr(it->name, "Antitoxin"))) ||
               it->category == ITEM_FUEL;
    case MAGIC_SHOP:
        return (it->category == ITEM_RING || it->category == ITEM_NECK ||
                it->category == ITEM_BACK || is_robe(it) ||
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
                (strstr(it->name, "Wand") || strstr(it->name, "Staff"))) ||
               is_staff(it);
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
static void fill_shop_by_specialization(MerchantData *m, int floor_id) {
    m->item_count = 0;
    int attempts = 0;
    int target = 50; // Max 50 items

    //Determine the maximum tier based on the plan
    int max_tier = (floor_id / 20) + 1; // Floor 0-19: Tier 1, 20-39: Tier 2…
    if (max_tier > 5) {
        max_tier = 5;
    }

    while (m->item_count < target && attempts < 5000) {
        int i_idx = rand() % item_database_size;
        if (is_item_matching_spec(i_idx, m->spec)) {
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

            if (item_tier <= max_tier + 1 || m->spec == BLACK_MARKET) {
                // Avoid duplicates
                bool dup = false;
                for (int j = 0; j < m->item_count; j++) {
                    if (m->item_templates[j] == i_idx) {
                        dup = true;
                    }
                }
                if (!dup) {
                    int stock = (item_database[i_idx].category == ITEM_CONSUMABLE)
                                    ? (5 + rand() % 10)
                                    : (1 + rand() % 3);
                    add_item_to_shop(m, i_idx, stock);
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
static void fill_provisioner_floor0(MerchantData *m) {
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
    m->item_count = 0;
    for (size_t i = 0; i < sizeof(k_stock) / sizeof(k_stock[0]); i++) {
        int idx = get_item_idx_by_name(k_stock[i].item);
        if (idx >= 0) {
            add_item_to_shop(m, idx, k_stock[i].stock);
        }
    }
    //If the DB is missing key items, fallback to random filling
    if (m->item_count < 5) {
        fill_shop_by_specialization(m, 0);
    }
}

/* =========================================================================
 * Public functions
 * ========================================================================= */

/*The shop of a new merchant (its entity_id is set): its trade and name.
 * The table of shops is empty when a world is generated, so it never
 * fails here; a merchant without one would sell nothing.*/
static MerchantData *open_shop(Game *game, NPC *n, MerchantSpecialization spec, const char *name) {
    static MerchantData no_shop;
    MerchantData *m = npc_shop_new(&game->pools, game->npcs, MAX_NPCS, n);
    if (!m) {
        memset(&no_shop, 0, sizeof(no_shop));
        return &no_shop;
    }
    m->spec = spec;
    copy_str(m->shop_name, name, sizeof(m->shop_name));
    return m;
}

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
    MerchantData *f_shop = open_shop(game, f, BLACKSMITH, "The Iron Anvil");
    fill_shop_by_specialization(f_shop, 0);

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
    MerchantData *p_shop = open_shop(game, p, ALCHEMIST, "The Bubbling Cauldron");
    fill_shop_by_specialization(p_shop, 0);

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
    MerchantData *o_shop = open_shop(game, o, PROVISIONER, "The Drunken Dragon");
    fill_provisioner_floor0(o_shop);

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
    MerchantData *s_shop = open_shop(game, s, MAGIC_SHOP, "Temple of Arcana");
    fill_shop_by_specialization(s_shop, 0);

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
    MerchantData *m_shop = open_shop(game, m, BLACK_MARKET, "The Shadow's Edge");
    fill_shop_by_specialization(m_shop, 40);
    m_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;
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
    MerchantData *m_shop = open_shop(game, m, MAGIC_SHOP, "Xanthus's Arcane Curios");
    fill_shop_by_specialization(m_shop, 20);
    m_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;

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
    MerchantData *p_shop = open_shop(game, p, MAGIC_SHOP, "Sister Elara's Sanctum");
    fill_shop_by_specialization(p_shop, 20);
    p_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;

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
    MerchantData *c_shop = open_shop(game, c, ALCHEMIST, "Master Malchor's Reagents");
    fill_shop_by_specialization(c_shop, 20);
    c_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;

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
    MerchantData *r_shop = open_shop(game, r, BOOKS_MAGE, "The Arcane Library");
    fill_shop_by_specialization(r_shop, 100);
    r_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;

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
    MerchantData *b_shop = open_shop(game, b, BOOKS_PRIEST, "Temple Library");
    fill_shop_by_specialization(b_shop, 100);
    b_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;
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
    MerchantData *a_shop = open_shop(game, a, BOOKS_MARTIAL, "The Archive of a Thousand Battles");
    fill_shop_by_specialization(a_shop, 100);
    a_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;
}

/*Tiles a creature must be able to reach from where it is placed.*/
#define SPAWN_MIN_REGION 20

/*True if at least SPAWN_MIN_REGION passable tiles join (x, y) in the four
 * directions monsters walk: room walls drawn on the diagonal leave cells
 * that only a diagonal touches, where a creature could never move.*/
static bool spawn_region_open(const Floor *fl, int x, int y) {
    int qx[SPAWN_MIN_REGION], qy[SPAWN_MIN_REGION];
    int n = 0;
    qx[n] = x;
    qy[n++] = y;
    for (int head = 0; head < n && n < SPAWN_MIN_REGION; head++) {
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4 && n < SPAWN_MIN_REGION; d++) {
            int nx = qx[head] + dx[d], ny = qy[head] + dy[d];
            if (nx < 1 || ny < 1 || nx >= MAP_WIDTH - 1 || ny >= MAP_HEIGHT - 1 ||
                !voxel_passable(fl->map.data[0][ny][nx])) {
                continue;
            }
            bool seen = false;
            for (int k = 0; k < n && !seen; k++) {
                seen = qx[k] == nx && qy[k] == ny;
            }
            if (!seen) {
                qx[n] = nx;
                qy[n++] = ny;
            }
        }
    }
    return n >= SPAWN_MIN_REGION;
}

/*A random tile of open ground (voxel_is_ground) with nobody on it, from
 * which a creature can walk away, in 200 tries; false if none was found.*/
static bool random_spawn_tile(Game *game, int f, int *x, int *y) {
    const Floor *fl = &game->world->floors[f];
    for (int tries = 0; tries < 200; tries++) {
        int tx = 1 + rand() % (MAP_WIDTH - 2);
        int ty = 1 + rand() % (MAP_HEIGHT - 2);
        if (voxel_is_ground(fl->map.data[0][ty][tx]) && fl->entity_grid[ty][tx] == 0 &&
            spawn_region_open(fl, tx, ty)) {
            *x = tx;
            *y = ty;
            return true;
        }
    }
    return false;
}

static bool is_boss_template(const MonsterTemplate *m) {
    return m->archetype && strcasecmp(m->archetype, "boss") == 0;
}

/*The bestiary entries whose floor range holds f: the guardians ("boss" in
 * the data) for a boss floor, every other creature for the rest. When the
 * range holds none of the kind, any creature of the range. The caller
 * frees the array (NULL when *size is 0).*/
static int *floor_monster_pool(int f, bool bosses, int *size) {
    int *pool = malloc((size_t)(bestiary_size > 0 ? bestiary_size : 1) * sizeof(int));
    *size = 0;
    for (int pass = 0; pool && pass < 2 && *size == 0; pass++) {
        for (int bi = 0; bi < bestiary_size; bi++) {
            const MonsterTemplate *m = &bestiary_data[bi];
            if (f >= m->floor_min && f <= m->floor_max &&
                (pass == 1 || is_boss_template(m) == bosses)) {
                pool[(*size)++] = bi;
            }
        }
    }
    if (*size == 0) {
        free(pool);
        return NULL;
    }
    return pool;
}

/*Fields shared by every new NPC of the dungeon; activation comes last,
 * by the caller, once template and archetype are final. The tile is taken
 * at once, so the next spawn does not land on it.*/
static NPC *dungeon_npc(Game *game, int *npc_idx, int f, int x, int y) {
    NPC *n = &game->npcs[(*npc_idx)++];
    n->entity_id = game->next_id++;
    n->floor_id = f;
    n->x = n->spawn_x = x;
    n->y = n->spawn_y = y;
    n->respawn_timer = 0;
    game->world->floors[f].entity_grid[y][x] = n->entity_id;
    return n;
}

/*The nearest free open ground to (ax, ay), ring by ring up to radius; false
 * if there is none.*/
static bool free_ground_near(const Floor *fl, int ax, int ay, int radius, int *ox, int *oy) {
    for (int r = 0; r <= radius; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int x = ax + dx, y = ay + dy;
                if ((abs(dx) != r && abs(dy) != r) || x < 1 || y < 1 || x >= MAP_WIDTH - 1 ||
                    y >= MAP_HEIGHT - 1) {
                    continue;
                }
                if (voxel_is_ground(fl->map.data[0][y][x]) && fl->entity_grid[y][x] == 0 &&
                    spawn_region_open(fl, x, y)) {
                    *ox = x;
                    *oy = y;
                    return true;
                }
            }
        }
    }
    return false;
}

bool spawn_floor_boss(Game *game, NPC *b, int f) {
    int size = 0;
    int *pool = floor_monster_pool(f, true, &size);
    int bx, by;
    if (!pool || !free_ground_near(&game->world->floors[f], MAP_CENTER_X, MAP_CENTER_Y,
                                   MAP_WIDTH / 2, &bx, &by)) {
        free(pool);
        return false;
    }
    int boss_id = pool[rand() % size];
    free(pool);
    memset(b, 0, sizeof(*b));
    b->entity_id = game->next_id++;
    b->floor_id = f;
    b->x = b->spawn_x = bx;
    b->y = b->spawn_y = by;
    b->archetype = ARCH_BOSS;
    /*template_idx is what a restart and a respawn read the template from.*/
    b->template_idx = boss_id;
    b->template = &bestiary_data[boss_id];
    snprintf(b->custom_name, sizeof(b->custom_name), "Boss %s", b->template->name);
    /*Five times the hit points and 50 more per floor: ai_init_npc.*/
    ai_init_npc(b, b->custom_name, b->floor_id);
    game->world->floors[f].entity_grid[by][bx] = b->entity_id;
    npc_set_active(game, b, true);
    return true;
}

/*100 monsters, 4 more per floor (at most 300), of the floor's pool.*/
static void spawn_monsters(Game *game, int *npc_idx, int f, const int *pool, int size) {
    int num_monsters = 100 + (f * 4);
    if (num_monsters > 300) {
        num_monsters = 300;
    }
    for (int m = 0; size > 0 && m < num_monsters && *npc_idx < MAX_NPCS; m++) {
        int px, py;
        if (!random_spawn_tile(game, f, &px, &py)) {
            continue;
        }
        int t_id = pool[rand() % size];
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
        if (!random_spawn_tile(game, f, &lx, &ly)) {
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

/*A wandering merchant of a random trade, on open ground (it looked for
 * stone floor only, which most biomes do not have: no merchant came).*/
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
    if (*npc_idx >= MAX_NPCS || !random_spawn_tile(game, f, &lx, &ly)) {
        return;
    }
    NPC *s = dungeon_npc(game, npc_idx, f, lx, ly);
    s->archetype = ARCH_MERCHANT;
    s->respawn_timer = -1;
    s->template_idx = 0;
    s->template = &bestiary_data[0];
    s->hp = 1;
    s->max_hp = 1;
    int trade = rand() % 8;
    MerchantData *s_shop = open_shop(game, s, k_trades[trade].spec, k_trades[trade].shop_name);
    s_shop->restock_timer = MERCHANT_RESTOCK_ROUNDS;
    npc_set_active(game, s, true); /*merchant: no floor-cache effect*/
    fill_shop_by_specialization(s_shop, f);
}

/*Populates every dungeon floor of a new world: a boss alone on every
 * tenth floor; elsewhere monsters, gold and chests, and a wandering
 * merchant on every fifth floor.*/
void populate_dungeons(Game *game) {
    int npc_idx = 11; // The 11 city merchants occupy slots 0-10
    for (int f = 1; f < MAX_FLOORS; f++) {
        if (f % 10 == 0) {
            if (npc_idx < MAX_NPCS && spawn_floor_boss(game, &game->npcs[npc_idx], f)) {
                npc_idx++;
            }
            continue;
        }
        int pool_size = 0;
        int *floor_pool = floor_monster_pool(f, false, &pool_size);
        spawn_monsters(game, &npc_idx, f, floor_pool, pool_size);
        spawn_treasures(game, &npc_idx, f);
        if (f % 5 == 0) {
            spawn_wandering_merchant(game, &npc_idx, f);
        }
        free(floor_pool);
    }
}
