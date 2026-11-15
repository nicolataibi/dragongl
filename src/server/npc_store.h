/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * npc_store.h - The shops of the merchants and the loot of the remains
 * and of the items on the ground, outside the NPC record.
 *
 * Few NPCs have a shop or loot (the merchants; the remains of characters
 * and the items dropped on the ground), so they live in two small tables
 * and the record keeps only their number (shop_id, loot_id; 0 = none).
 * An entry belongs to the entity_id that took it: a record that names an
 * entry taken since by another NPC (a slot reused, a record copied) sees
 * none. Nothing has to be freed by hand: when a table is full, the
 * entries that no NPC names any more are taken back.
 */

#ifndef DRAGONGL_NPC_STORE_H
#define DRAGONGL_NPC_STORE_H

#include <stdbool.h>

#include "server_entities.h"

#define MAX_SHOPS     256  /*merchants of the town and of the dungeon*/
#define MAX_LOOT_BAGS 1024 /*remains and items on the ground at once*/

typedef struct {
    int owner; /*entity_id of the merchant; 0 = free*/
    MerchantData shop;
} ShopEntry;

typedef struct {
    int owner; /*entity_id of the NPC that holds it; 0 = free*/
    ItemInstance items[NPC_LOOT_MAX];
} LootBag;

typedef struct {
    ShopEntry *shops; /*MAX_SHOPS*/
    LootBag *bags;    /*MAX_LOOT_BAGS*/
} NpcPools;

bool npc_pools_create(NpcPools *p);
void npc_pools_destroy(NpcPools *p);
/*Every entry free (a new world, a load).*/
void npc_pools_clear(NpcPools *p);

/*The shop of n, or NULL (not a merchant, or a stale number).*/
MerchantData *npc_shop(const NpcPools *p, const NPC *n);
/*Never NULL: the shop of n, or an empty one (it sells nothing, and what
 * is written into it is lost) for an NPC without a shop.*/
MerchantData *npc_shop_any(const NpcPools *p, const NPC *n);
/*A new empty shop for n (its entity_id must be set); NULL if the table
 * is full even after taking back the entries no NPC of npcs names.*/
MerchantData *npc_shop_new(NpcPools *p, const NPC *npcs, int max_npcs, NPC *n);

/*The NPC_LOOT_MAX items carried by n, or NULL.*/
ItemInstance *npc_loot(const NpcPools *p, const NPC *n);
/*New empty loot for n (every slot template -1); NULL if full. An entry
 * is taken back only from an inactive NPC or one that no longer names it.*/
ItemInstance *npc_loot_new(NpcPools *p, const NPC *npcs, int max_npcs, NPC *n);
/*n drops its loot (the entry is free again).*/
void npc_loot_release(NpcPools *p, NPC *n);

#endif
