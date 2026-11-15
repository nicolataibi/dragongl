/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * npc_store.c - The shops and the loot of the NPCs (see npc_store.h).
 */

#include "npc_store.h"

#include <stdlib.h>
#include <string.h>

bool npc_pools_create(NpcPools *p) {
    p->shops = calloc(MAX_SHOPS, sizeof(ShopEntry));
    p->bags = calloc(MAX_LOOT_BAGS, sizeof(LootBag));
    if (!p->shops || !p->bags) {
        npc_pools_destroy(p);
        return false;
    }
    return true;
}

void npc_pools_destroy(NpcPools *p) {
    free(p->shops);
    free(p->bags);
    p->shops = NULL;
    p->bags = NULL;
}

void npc_pools_clear(NpcPools *p) {
    for (int i = 0; i < MAX_SHOPS; i++) {
        p->shops[i].owner = 0;
    }
    for (int i = 0; i < MAX_LOOT_BAGS; i++) {
        p->bags[i].owner = 0;
    }
}

MerchantData *npc_shop(const NpcPools *p, const NPC *n) {
    int id = n->shop_id;
    if (id < 1 || id > MAX_SHOPS || n->entity_id == 0 || p->shops[id - 1].owner != n->entity_id) {
        return NULL;
    }
    return &p->shops[id - 1].shop;
}

MerchantData *npc_shop_any(const NpcPools *p, const NPC *n) {
    static MerchantData empty;
    MerchantData *m = npc_shop(p, n);
    if (m) {
        return m;
    }
    memset(&empty, 0, sizeof(empty));
    return &empty;
}

ItemInstance *npc_loot(const NpcPools *p, const NPC *n) {
    int id = n->loot_id;
    if (id < 1 || id > MAX_LOOT_BAGS || n->entity_id == 0 ||
        p->bags[id - 1].owner != n->entity_id) {
        return NULL;
    }
    return p->bags[id - 1].items;
}

/*The entries of a full table that no NPC names any more go back: a shop
 * stays while any NPC names it, loot only while an active NPC does.*/
static void reclaim(NpcPools *p, const NPC *npcs, int max_npcs, bool shops) {
    int cap = shops ? MAX_SHOPS : MAX_LOOT_BAGS;
    bool *named = calloc((size_t)cap, sizeof(bool));
    if (!named) {
        return;
    }
    for (int i = 0; i < max_npcs; i++) {
        const NPC *n = &npcs[i];
        if (shops && npc_shop(p, n)) {
            named[n->shop_id - 1] = true;
        } else if (!shops && n->active && npc_loot(p, n)) {
            named[n->loot_id - 1] = true;
        }
    }
    for (int i = 0; i < cap; i++) {
        if (!named[i]) {
            if (shops) {
                p->shops[i].owner = 0;
            } else {
                p->bags[i].owner = 0;
            }
        }
    }
    free(named);
}

/*A free entry (taking back the unnamed ones when there is none): its
 * index, or -1.*/
static int free_entry(NpcPools *p, const NPC *npcs, int max_npcs, bool shops) {
    for (int pass = 0; pass < 2; pass++) {
        int cap = shops ? MAX_SHOPS : MAX_LOOT_BAGS;
        for (int i = 0; i < cap; i++) {
            if ((shops ? p->shops[i].owner : p->bags[i].owner) == 0) {
                return i;
            }
        }
        if (pass == 0) {
            reclaim(p, npcs, max_npcs, shops);
        }
    }
    return -1;
}

MerchantData *npc_shop_new(NpcPools *p, const NPC *npcs, int max_npcs, NPC *n) {
    MerchantData *old = npc_shop(p, n);
    if (old) {
        memset(old, 0, sizeof(*old));
        return old;
    }
    int i = free_entry(p, npcs, max_npcs, true);
    if (i < 0 || n->entity_id == 0) {
        return NULL;
    }
    memset(&p->shops[i], 0, sizeof(p->shops[i]));
    p->shops[i].owner = n->entity_id;
    n->shop_id = i + 1;
    return &p->shops[i].shop;
}

ItemInstance *npc_loot_new(NpcPools *p, const NPC *npcs, int max_npcs, NPC *n) {
    ItemInstance *items = npc_loot(p, n);
    if (!items) {
        int i = free_entry(p, npcs, max_npcs, false);
        if (i < 0 || n->entity_id == 0) {
            return NULL;
        }
        p->bags[i].owner = n->entity_id;
        n->loot_id = i + 1;
        items = p->bags[i].items;
    }
    memset(items, 0, sizeof(ItemInstance) * NPC_LOOT_MAX);
    for (int k = 0; k < NPC_LOOT_MAX; k++) {
        items[k].template_idx = -1;
    }
    return items;
}

void npc_loot_release(NpcPools *p, NPC *n) {
    if (npc_loot(p, n)) {
        p->bags[n->loot_id - 1].owner = 0;
    }
    n->loot_id = 0;
}
