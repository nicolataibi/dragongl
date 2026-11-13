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
 * server_loot.c - Loot: the unique artifacts and what a dead monster drops.
 */

#include "server_loot.h"
#include <limits.h>
#include "server_internal.h"

#include "player.h"
#include "server_log.h"

//Each artifact can only drop ONE time in the entire life of the server.
#define MAX_ARTIFACTS 58
typedef struct {
    const char *name;           //name of the artifact
    int base_template_name_idx; //searched by name in item_database
    int to_hit;                 //to-hit bonus
    int to_dam;                 //bonus to-dam
    int ac_bonus;               //AC bonuses
    ItemElement element;        //special element
    ItemBlessing blessing;      //blessing
    int str_bonus;              //STR bonus
    int dex_bonus;              //DEX bonuses
    int con_bonus;              //CON bonus
    bool already_dropped;       //true = already dropped, don't drop again
} ArtifactDef;

static ArtifactDef artifact_registry[MAX_ARTIFACTS] = {
    {"The Void Blade", -1, 5, 8, 0, ELEM_SHOCKING, BLESS_BLESSED, 2, 0, 0, false},
    {"The Shield of Aegis", -1, 0, 0, 7, ELEM_NONE, BLESS_BLESSED, 0, 1, 3, false},
    {"The Staff of Dawn", -1, 3, 5, 0, ELEM_FLAMING, BLESS_BLESSED, 0, 0, 1, false},
    {"The Shadow Robe", -1, 0, 0, 5, ELEM_NONE, BLESS_NORMAL, 0, 3, 0, false},
    {"The Poisoned Dagger", -1, 4, 3, 0, ELEM_POISONOUS, BLESS_NORMAL, 0, 2, 0, false},
    {"The Frost Gauntlet", -1, 2, 6, 2, ELEM_FROST, BLESS_NORMAL, 0, 0, 0, false},
    {"Leviathan's Armor", -1, 0, 0, 10, ELEM_NONE, BLESS_BLESSED, 1, 0, 4, false},
    {"The Bow of Destiny", -1, 6, 4, 0, ELEM_SHOCKING, BLESS_BLESSED, 0, 3, 0, false},
    {"Giant's Cleaver", -1, 2, 9, 0, ELEM_NONE, BLESS_NORMAL, 4, 0, 0, false},
    {"Sage's Ring", -1, 0, 0, 0, ELEM_NONE, BLESS_BLESSED, 0, 0, 1, false},
    {"Boots of the Wind", -1, 0, 0, 1, ELEM_NONE, BLESS_NORMAL, 0, 4, 0, false},
    {"Amulet of Life", -1, 0, 0, 0, ELEM_NONE, BLESS_BLESSED, 0, 0, 5, false},
    {"Hero's Cloak", -1, 1, 1, 2, ELEM_NONE, BLESS_BLESSED, 1, 1, 1, false},
    {"Thunder Hammer", -1, 3, 7, 0, ELEM_SHOCKING, BLESS_BLESSED, 3, 0, 0, false},
    {"Vigilante's Helm", -1, 0, 0, 3, ELEM_NONE, BLESS_NORMAL, 0, 0, 2, false},
    {"Bracers of Strength", -1, 0, 2, 1, ELEM_NONE, BLESS_NORMAL, 3, 0, 0, false},
    {"Sword of Fire", -1, 4, 4, 0, ELEM_FLAMING, BLESS_NORMAL, 1, 0, 0, false},
    {"Belt of Vigor", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 4, false},
    {"Book of Mysteries", -1, 1, 0, 0, ELEM_NONE, BLESS_BLESSED, 0, 0, 0, false},
    {"Shield of the Eclipse", -1, 0, 0, 6, ELEM_NONE, BLESS_NORMAL, 0, 0, 2, false},
    {"Assassin's Dagger", -1, 5, 5, 0, ELEM_POISONOUS, BLESS_NORMAL, 0, 4, 0, false},
    {"Iron Breastplate", -1, 0, 0, 8, ELEM_NONE, BLESS_NORMAL, 2, 0, 2, false},
    {"Elven Longbow", -1, 5, 2, 0, ELEM_NONE, BLESS_BLESSED, 0, 3, 0, false},
    {"Wizard's Staff", -1, 2, 2, 0, ELEM_NONE, BLESS_BLESSED, 0, 0, 1, false},
    {"Ring of Fire", -1, 0, 2, 0, ELEM_FLAMING, BLESS_NORMAL, 0, 0, 0, false},
    {"Hunter's Boots", -1, 1, 1, 1, ELEM_NONE, BLESS_NORMAL, 0, 2, 0, false},
    {"Royal Cloak", -1, 0, 0, 2, ELEM_NONE, BLESS_BLESSED, 0, 1, 2, false},
    {"Fighter's Gloves", -1, 2, 2, 0, ELEM_NONE, BLESS_NORMAL, 2, 2, 0, false},
    {"Crown of the Sovereign", -1, 1, 0, 1, ELEM_NONE, BLESS_BLESSED, 0, 0, 2, false},
    {"Spear of Destiny", -1, 4, 4, 0, ELEM_NONE, BLESS_BLESSED, 2, 2, 0, false},
    {"Battleaxe", -1, 1, 8, 0, ELEM_NONE, BLESS_NORMAL, 3, 0, 0, false},
    {"Paladin's Shield", -1, 0, 0, 5, ELEM_NONE, BLESS_BLESSED, 0, 0, 3, false},
    {"Talisman of Fortune", -1, 1, 1, 1, ELEM_NONE, BLESS_BLESSED, 1, 1, 1, false},
    {"Lead Boots", -1, 0, 0, 3, ELEM_NONE, BLESS_NORMAL, 2, -2, 2, false},
    {"Cloak of Darkness", -1, 2, 0, 2, ELEM_NONE, BLESS_NORMAL, 0, 3, 0, false},
    {"Sword of the Righteous", -1, 3, 3, 0, ELEM_NONE, BLESS_BLESSED, 2, 0, 0, false},
    {"Protection Ring", -1, 0, 0, 2, ELEM_NONE, BLESS_BLESSED, 0, 0, 0, false},
    {"Leather Bracelets", -1, 0, 0, 2, ELEM_NONE, BLESS_NORMAL, 0, 2, 0, false},
    {"Steel helmet", -1, 0, 0, 4, ELEM_NONE, BLESS_NORMAL, 0, 0, 1, false},
    {"Arrows of Light", -1, 3, 3, 0, ELEM_SHOCKING, BLESS_BLESSED, 0, 0, 0, false},
    {"Woodland Staff", -1, 2, 2, 0, ELEM_NONE, BLESS_NORMAL, 1, 1, 0, false},
    {"Wanderer's Cloak", -1, 0, 0, 1, ELEM_NONE, BLESS_NORMAL, 0, 2, 0, false},
    {"Ring of Power", -1, 1, 1, 0, ELEM_NONE, BLESS_BLESSED, 1, 0, 0, false},
    {"Rubber Boots", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 1, 0, false},
    {"Warrior Belt", -1, 0, 1, 0, ELEM_NONE, BLESS_NORMAL, 2, 0, 0, false},
    {"Velvet Gloves", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Silver Medallion", -1, 0, 0, 1, ELEM_NONE, BLESS_BLESSED, 0, 0, 0, false},
    {"Silver Dagger", -1, 2, 2, 0, ELEM_NONE, BLESS_BLESSED, 0, 1, 0, false},
    {"Wooden Bow", -1, 2, 1, 0, ELEM_NONE, BLESS_NORMAL, 0, 1, 0, false},
    {"Wooden Shield", -1, 0, 0, 2, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Leather Helm", -1, 0, 0, 1, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Wool cloak", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 1, false},
    {"Copper Ring", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Leather Boots", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 1, 0, false},
    {"Rope Belt", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Linen gloves", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Stone Necklace", -1, 0, 0, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false},
    {"Iron Sword", -1, 1, 1, 0, ELEM_NONE, BLESS_NORMAL, 0, 0, 0, false}};
static int artifact_count = 58;

void artifacts_load(void) {
    char path[STATE_DIR_MAX + 32];
    bool flags[MAX_ARTIFACTS] = {false};
    char why[96];
    if (!state_path(path, sizeof(path), "artifacts.dat")) {
        return;
    }
    BinFileStatus st = persist_read_flags(path, flags, artifact_count, why, sizeof(why));
    if (st == BINFILE_INVALID) {
        server_log(
            "SYS",
            "WARNING: artifacts.dat unreadable (%s): every unique artifact is available again.",
            why);
        return;
    }
    if (st == BINFILE_OK) {
        for (int i = 0; i < artifact_count; i++) {
            artifact_registry[i].already_dropped = flags[i];
        }
    }
}

void artifacts_save(void) {
    char path[STATE_DIR_MAX + 32];
    bool flags[MAX_ARTIFACTS];
    if (!state_path(path, sizeof(path), "artifacts.dat")) {
        return;
    }
    for (int i = 0; i < artifact_count; i++) {
        flags[i] = artifact_registry[i].already_dropped;
    }
    persist_write_flags(path, flags, artifact_count); /*failures: logged by the writer*/
}

/*Quality of a weapon or armor from a magic tier: 20% masterwork, then 20%
 * fine (a second roll only when the first fails), else normal.*/
static ItemQuality roll_good_quality(void) {
    if (rand() % 100 < 20) {
        return QUALITY_MASTERWORK;
    }
    if (rand() % 100 < 20) {
        return QUALITY_FINE;
    }
    return QUALITY_NORMAL;
}

/* -----------------------------------------------------------------------
 * LOOT SYSTEM: a dead monster drops, LOOT_DROP_CHANCE times in 100, one item
 * whose rarity comes from the depth of the floor and whose category leans
 * on the monster's archetype.
 * ----------------------------------------------------------------------- */

#define RARITY_TIERS 9

/*Cost bands (gp) that approximate the rarity of an item.*/
static const uint64_t k_rarity_min[RARITY_TIERS] = {0,      100,    1000,   5000,   20000,
                                                    100000, 250000, 500000, 1000000};
static const uint64_t k_rarity_max[RARITY_TIERS] = {99,     999,    4999,   19999,     99999,
                                                    249999, 499999, 999999, UINT64_MAX};
static const char *const k_rarity_name[RARITY_TIERS] = {"MUNDANE", "COMMON",       "UNCOMMON",
                                                        "RARE",    "VERY_RARE",    "LEGENDARY",
                                                        "EPIC",    "MYTHIC_RELIC", "ARTIFACT"};

/*Rarity tier by depth: a d100 against the three thresholds of the band
 * gives first_tier, +1, +2 or +3.*/
static int loot_rarity_tier(int floor_id) {
    static const struct {
        int max_floor;
        int first_tier;
        int below[3];
    } k_bands[] = {
        {10, 0, {60, 90, 100}},     {30, 0, {30, 60, 85}}, {50, 1, {15, 45, 80}},
        {70, 2, {15, 45, 80}},      {85, 3, {15, 45, 80}}, {95, 4, {15, 45, 80}},
        {INT_MAX, 5, {15, 45, 80}},
    };
    int roll = rand() % 100;
    int band = 0;
    while (floor_id > k_bands[band].max_floor) {
        band++;
    }
    int tier = k_bands[band].first_tier;
    for (int i = 0; i < 3 && roll >= k_bands[band].below[i]; i++) {
        tier++;
    }
    return tier < RARITY_TIERS ? tier : RARITY_TIERS - 1;
}

/*The category a monster tends to carry, or -1.*/
static int loot_preferred_category(const NPC *killer) {
    switch (killer->archetype) {
    case ARCH_MELEE:
    case ARCH_BRUTE:
        return (rand() % 2 == 0) ? ITEM_WEAPON : ITEM_ARMOR;
    case ARCH_ASSASSIN:
        return ITEM_WEAPON;
    case ARCH_CASTER:
        return ITEM_CONSUMABLE;
    default:
        return -1;
    }
}

/*A random item of the tier's cost band: of the preferred category if
 * there is one, else of any; any item at all when the band is empty.*/
static int loot_pick_template(int tier, int preferred_cat) {
    enum { MAX_LOOT_CANDIDATES = 1024 };
    int candidates[MAX_LOOT_CANDIDATES];
    int n = 0;
    for (int pass = 0; pass < 2 && n == 0; pass++) {
        for (int i = 0; i < item_database_size && n < MAX_LOOT_CANDIDATES; i++) {
            uint64_t cost = item_database[i].cost;
            if (cost < k_rarity_min[tier] || cost > k_rarity_max[tier]) {
                continue;
            }
            if (pass == 0 && preferred_cat != -1 &&
                (int)item_database[i].category != preferred_cat) {
                continue;
            }
            candidates[n++] = i;
        }
    }
    if (n == 0) {
        return rand() % item_database_size;
    }
    return candidates[rand() % n];
}

/*Adds the drop to a stack already in the backpack; false if there is none.*/
static bool loot_stack(Client *c, const NPC *killer, int idx, bool identified) {
    const ItemTemplate *it = &item_database[idx];
    if (it->max_stack <= 1) {
        return false;
    }
    for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == idx && c->backpack[i].stack_count < it->max_stack) {
            c->backpack[i].stack_count++;
            send_text_to_client(c->sock, "[LOOT] %s dropped: %s (x%d)", npc_name(killer),
                                identified ? it->name : "??? (unknown)",
                                c->backpack[i].stack_count);
            return true;
        }
    }
    return false;
}

static ItemQuality loot_quality(int tier) {
    if (tier >= 1) {
        return roll_good_quality();
    }
    return (rand() % 100 < 30) ? QUALITY_RUSTY : QUALITY_NORMAL;
}

/*10% blessed, else 10% cursed.*/
static ItemBlessing loot_blessing(void) {
    if (rand() % 100 < 10) {
        return BLESS_BLESSED;
    }
    return rand() % 100 < 10 ? BLESS_CURSED : BLESS_NORMAL;
}

/*The dropped instance: weapons and armor get quality, bonuses, an element
 * and a blessing from the tier. RARE and above drop unidentified.*/
static ItemInstance loot_instance(int idx, int tier, bool identified) {
    const ItemTemplate *it = &item_database[idx];
    ItemInstance drop;
    memset(&drop, 0, sizeof(ItemInstance));
    drop.template_idx = idx;
    drop.durability = (it->max_durability > 0) ? it->max_durability : 100;
    if (it->category == ITEM_LIGHT_SOURCE && strcasestr(it->name, "lantern")) {
        drop.durability = 5000;
    }
    drop.stack_count = 1;
    drop.is_identified = identified;
    drop.quality = QUALITY_NORMAL;
    drop.blessing = BLESS_NORMAL;
    drop.element = ELEM_NONE;

    if (it->category == ITEM_WEAPON) {
        drop.quality = loot_quality(tier);
        drop.to_hit_bonus = (rand() % (tier + 1));
        drop.to_dam_bonus = tier + (rand() % 2);
        if (tier >= 2 && rand() % 100 < 30) {
            drop.element = (ItemElement)((rand() % 4) + 1);
        }
        drop.blessing = loot_blessing();
    } else if (it->category == ITEM_ARMOR || it->category == ITEM_SHIELD) {
        drop.quality = loot_quality(tier);
        drop.ac_bonus = tier;
        drop.blessing = loot_blessing();
    }
    return drop;
}

/*The base item of an artifact: the first weapon, armor or shield worth
 * 100000 gp or more; fallback when there is none.*/
static int artifact_base_template(int fallback) {
    for (int ti = 0; ti < item_database_size; ti++) {
        if (item_database[ti].cost >= 100000 && (item_database[ti].category == ITEM_WEAPON ||
                                                 item_database[ti].category == ITEM_ARMOR ||
                                                 item_database[ti].category == ITEM_SHIELD)) {
            return ti;
        }
    }
    return fallback;
}

/*The next unique artifact that never dropped goes to the player; false
 * when every one already has.*/
static bool loot_artifact(Client *c, const NPC *killer, int fallback_idx) {
    ArtifactDef *def = NULL;
    for (int ai = 0; ai < artifact_count && !def; ai++) {
        if (!artifact_registry[ai].already_dropped) {
            def = &artifact_registry[ai];
        }
    }
    if (!def) {
        return false;
    }
    int base_idx = artifact_base_template(fallback_idx);
    ItemInstance art;
    memset(&art, 0, sizeof(ItemInstance));
    art.template_idx = base_idx;
    art.durability =
        (item_database[base_idx].max_durability > 0) ? item_database[base_idx].max_durability : 100;
    art.stack_count = 1;
    art.is_identified = true; //the artefacts are immediately recognisable
    art.is_artifact = true;
    copy_str(art.artifact_name, def->name, sizeof(art.artifact_name));
    art.to_hit_bonus = def->to_hit;
    art.to_dam_bonus = def->to_dam;
    art.ac_bonus = def->ac_bonus;
    art.element = def->element;
    art.blessing = def->blessing;
    art.artifact_str_bonus = def->str_bonus;
    art.artifact_dex_bonus = def->dex_bonus;
    art.artifact_con_bonus = def->con_bonus;
    art.quality = QUALITY_MASTERWORK;
    //Mark as dropped and save immediately (it's a global event!)
    def->already_dropped = true;
    artifacts_save();
    c->backpack[c->backpack_count++] = art;
    char abuf[160];
    get_full_item_name(&art, abuf, sizeof(abuf));
    send_text_to_client(c->sock,
                        "[*** ARTIFACT ***] %s has surrendered the legendary: %s"
                        "-- FIRST AND ONLY IN THE WORLD!",
                        npc_name(killer), abuf);
    return true;
}

void drop_loot_from_monster(Client *c, NPC *killer) {
    if (!c || !killer) {
        return;
    }
    if ((rand() % 100) >= LOOT_DROP_CHANCE) {
        return;
    }
    if (c->backpack_count >= MAX_BACKPACK) {
        send_text_to_client(c->sock, "[LOOT] Backpack full! Lose %s loot.",
                            killer->template ? killer->template->name : "treasure");
        return;
    }
    int tier = loot_rarity_tier(killer->floor_id);
    int chosen_idx = loot_pick_template(tier, loot_preferred_category(killer));
    bool identified = (tier <= 2);
    if (loot_stack(c, killer, chosen_idx, identified)) {
        return;
    }
    ItemInstance drop = loot_instance(chosen_idx, tier, identified);

    /*LEGENDARY and above: one time in four a unique artifact instead.*/
    if (tier >= 5 && rand() % 100 < 25 && loot_artifact(c, killer, chosen_idx)) {
        return;
    }
    c->backpack[c->backpack_count++] = drop;
    char buf[128];
    get_full_item_name(&drop, buf, sizeof(buf));
    send_text_to_client(c->sock, "[LOOT] %s dropped: %s [%s]", npc_name(killer), buf,
                        k_rarity_name[tier]);
}
