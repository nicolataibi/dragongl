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

/*data_items.c - Loader of data/items.json.*/

#include "strnum.h"
#include "data_internal.h"
#include "items.h"
#include "classes.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static MaterialType string_to_material_type(const char *str) {
    if (!str) {
        return MAT_NONE;
    }
    if (strcasecmp(str, "cloth") == 0) {
        return MAT_CLOTH;
    }
    if (strcasecmp(str, "leather") == 0) {
        return MAT_LEATHER;
    }
    if (strcasecmp(str, "wood") == 0) {
        return MAT_WOOD;
    }
    if (strcasecmp(str, "bone") == 0) {
        return MAT_BONE;
    }
    if (strcasecmp(str, "stone") == 0) {
        return MAT_STONE;
    }
    if (strcasecmp(str, "iron") == 0) {
        return MAT_IRON;
    }
    if (strcasecmp(str, "steel") == 0) {
        return MAT_STEEL;
    }
    if (strcasecmp(str, "mithril") == 0) {
        return MAT_MITHRIL;
    }
    if (strcasecmp(str, "glass") == 0) {
        return MAT_GLASS;
    }
    if (strcasecmp(str, "paper") == 0) {
        return MAT_PAPER;
    }
    return MAT_NONE;
}

//Loading objects from JSON
/*The JSON fields of one item, as the category parsers read them.*/
typedef struct {
    cJSON *item, *name, *desc, *category, *classification, *ac, *damage, *properties, *cost;
} ItemJson;

/*Weapons and ammunition: damage dice and the weapon properties.*/
static void item_weapon(ItemTemplate *t, const ItemJson *j) {
    bool is_ammo = false;
    if (j->name) {
        if (cJSON_IsString(j->name)) {
            if (data_strcasestr(j->name->valuestring, "arrow")) {
                is_ammo = true;
            }
            if (data_strcasestr(j->name->valuestring, "bolt")) {
                is_ammo = true;
            }
            if (data_strcasestr(j->name->valuestring, "bullet")) {
                is_ammo = true;
            }
            if (data_strcasestr(j->name->valuestring, "needle")) {
                is_ammo = true;
            }
        }
    }
    if (is_ammo) {
        t->category = ITEM_AMMO;
        t->max_stack = 20;
        t->weight = 0.05f;
    } else {
        t->category = ITEM_WEAPON;
        t->max_durability = 100;
        t->weight = 3.0f;
        if (j->properties) {
            if (cJSON_IsString(j->properties)) {
                if (data_strcasestr(j->properties->valuestring, "heavy")) {
                    t->weight = 6.0f;
                }
                if (data_strcasestr(j->properties->valuestring, "two-handed")) {
                    t->weight = 6.0f;
                }
                if (data_strcasestr(j->properties->valuestring, "light")) {
                    t->weight = 2.0f;
                }
                if (data_strcasestr(j->properties->valuestring, "finesse")) {
                    t->weight = 2.0f;
                }
            }
        }
    }
}

/*Armor and shields: base AC by weight class, shield bonus.*/
static void item_armor(ItemTemplate *t, const ItemJson *j) {
    bool is_shield = false;
    if (j->classification) {
        if (cJSON_IsString(j->classification)) {
            if (strcmp(j->classification->valuestring, "Shield") == 0) {
                is_shield = true;
            }
        }
    }
    if (j->name) {
        if (cJSON_IsString(j->name)) {
            if (data_strcasestr(j->name->valuestring, "shield")) {
                is_shield = true;
            }
        }
    }
    if (is_shield) {
        t->category = ITEM_SHIELD;
        t->ac_bonus = 2;
        t->max_durability = 100;
        t->weight = 5.0f;
    } else {
        t->category = ITEM_ARMOR;
        t->max_durability = 200;
        t->weight = 15.0f;
        if (j->classification) {
            if (cJSON_IsString(j->classification)) {
                if (data_strcasestr(j->classification->valuestring, "Heavy")) {
                    t->weight = 55.0f;
                }
                if (data_strcasestr(j->classification->valuestring, "Medium")) {
                    t->weight = 20.0f;
                }
                if (data_strcasestr(j->classification->valuestring, "Light")) {
                    t->weight = 10.0f;
                }
            }
        }
    }
}

/*Potions and oils: what they heal or grant.*/
static void item_potion(ItemTemplate *t, const ItemJson *j) {
    bool is_oil = false;
    if (j->name) {
        if (cJSON_IsString(j->name)) {
            if (data_strcasestr(j->name->valuestring, "oil")) {
                if (!data_strcasestr(j->name->valuestring, "potion")) {
                    is_oil = true;
                }
            }
        }
    }
    if (is_oil) {
        t->category = ITEM_FUEL;
        t->duration_turns = 8000;
        t->max_durability = 8000;
        t->max_stack = 1;
        t->weight = 1.0f;
    } else {
        t->category = ITEM_CONSUMABLE;
        t->max_stack = 5;
        t->weight = 1.0f;
        t->needs_id = true;
        if (j->name) {
            if (cJSON_IsString(j->name)) {
                if (data_strcasestr(j->name->valuestring, "Healing")) {
                    if (data_strcasestr(j->name->valuestring, "Greater")) {
                        t->heal_amount = 40;
                    } else if (data_strcasestr(j->name->valuestring, "Superior")) {
                        t->heal_amount = 80;
                    } else if (data_strcasestr(j->name->valuestring, "Supreme")) {
                        t->heal_amount = 120;
                    } else {
                        t->heal_amount = 20;
                    }
                }
            }
        }
    }
}

/*Adventuring gear: light sources, food, fuel, tools.*/
static void item_gear(ItemTemplate *t, const ItemJson *j) {
    bool is_torch = false;
    bool is_lantern = false;
    bool is_ration = false;
    bool is_oil = false;
    if (j->name) {
        if (cJSON_IsString(j->name)) {
            if (data_strcasestr(j->name->valuestring, "torch")) {
                is_torch = true;
            }
            if (data_strcasestr(j->name->valuestring, "lantern")) {
                is_lantern = true;
            }
            if (data_strcasestr(j->name->valuestring, "ration")) {
                is_ration = true;
            }
            if (strcmp(j->name->valuestring, "Oil (flask)") == 0) {
                is_oil = true;
            }
        }
    }
    if (is_torch) {
        t->category = ITEM_LIGHT_SOURCE;
        t->light_radius = 6;
        t->duration_turns = 1000;
        t->max_durability = 1000;
        t->max_stack = 10;
        t->weight = 1.0f;
    } else if (is_lantern) {
        t->category = ITEM_LIGHT_SOURCE;
        t->light_radius = 12;
        t->duration_turns = 8000;
        t->max_durability = 8000;
        t->max_stack = 1;
        t->weight = 2.0f;
    } else if (is_ration) {
        t->category = ITEM_CONSUMABLE;
        t->heal_amount = 5;
        t->max_stack = 5;
        t->weight = 1.0f;
    } else if (is_oil) {
        t->category = ITEM_FUEL;
        t->duration_turns = 8000;
        t->max_durability = 8000;
        t->max_stack = 10;
        t->weight = 1.0f;
    } else {
        t->category = ITEM_MISC;
        t->weight = 1.0f;
    }
}

/*Spell books: the spells they teach and the classes that can learn them.*/
static void item_book(ItemTemplate *t, const ItemJson *j) {
    /*-------------------------------------------------------
     * Magic Books / Spellbooks / Grimoires
     * The "properties" field contains "LEVEL_X_Y" where X and Y
     * are the minimum and maximum levels of the spells contained.
     * The "book_class_mask" field is the bitmask of the classes
     * (ClassType bits, see include/classes.h) that can read and
     * learn spells from this book.
     * -------------------------------------------------------*/
    t->category = ITEM_BOOK;
    t->weight = 2.0f;
    t->max_stack = 1;
    t->needs_id = false;

    /* Spell levels from the LEVEL_X_Y property (books without a spell list) */
    cJSON *props = cJSON_GetObjectItem(j->item, "properties");
    if (props && cJSON_IsString(props)) {
        const char *p = props->valuestring;
        const char *level_tag = data_strcasestr(p, "LEVEL_");
        if (level_tag) {
            int mn = 0;
            int mx = 0;
            int lv[2] = {0, 0};
            scan_ints(level_tag + 6, '_', 2, lv);
            mn = lv[0];
            mx = lv[1];
            t->book_min_level = mn;
            t->book_max_level = mx;
        }
    }

    /*book_seq: book sequence in the class series (0-5)*/
    cJSON *bseq = cJSON_GetObjectItem(j->item, "book_seq");
    if (bseq && cJSON_IsNumber(bseq)) {
        t->book_seq = (int)bseq->valuedouble;
    } else {
        t->book_seq = 0;
    }

    /*spells: Array of spell names sorted by ascending level*/
    t->book_spell_count = 0;
    cJSON *spells_arr = cJSON_GetObjectItem(j->item, "spells");
    if (spells_arr && cJSON_IsArray(spells_arr)) {
        int sc = cJSON_GetArraySize(spells_arr);
        if (sc > MAX_BOOK_SPELLS) {
            sc = MAX_BOOK_SPELLS;
        }
        for (int si = 0; si < sc; si++) {
            cJSON *sp_name = cJSON_GetArrayItem(spells_arr, si);
            if (sp_name && cJSON_IsString(sp_name)) {
                strncpy(t->book_spell_names[si], sp_name->valuestring, 63);
                t->book_spell_names[si][63] = '\0';
                t->book_spell_count++;
            }
        }
    }

    /* Class mask: read exclusively from the "book_class_mask"
     * JSON field (bitmask of ClassType, see include/classes.h).
     * Books without a valid mask stay readable by every class. */
    cJSON *mask_field = cJSON_GetObjectItem(j->item, "book_class_mask");
    if (mask_field && cJSON_IsNumber(mask_field) && mask_field->valueint > 0) {
        t->book_class_mask = (uint32_t)mask_field->valueint;
    } else {
        t->book_class_mask = (1u << CLASS_COUNT) - 1;
    }
}

/*Wondrous items: rings, amulets, wands and other magic.*/
static void item_wondrous(ItemTemplate *t, const ItemJson *j) {
    t->needs_id = true;
    t->weight = 1.0f;
    if (j->name) {
        if (cJSON_IsString(j->name)) {
            const char *n_str = j->name->valuestring;
            if (data_strcasestr(n_str, "Helm") || data_strcasestr(n_str, "Hat") ||
                data_strcasestr(n_str, "Crown")) {
                t->category = ITEM_HEAD;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (data_strcasestr(n_str, "Amulet") || data_strcasestr(n_str, "Brooch") ||
                       data_strcasestr(n_str, "Necklace") || data_strcasestr(n_str, "Periapt")) {
                t->category = ITEM_NECK;
                t->weight = 0.5f;
            } else if (data_strcasestr(n_str, "Cloak") || data_strcasestr(n_str, "Cape") ||
                       data_strcasestr(n_str, "Mantle")) {
                t->category = ITEM_BACK;
                t->weight = 1.0f;
            } else if (data_strcasestr(n_str, "Gloves") || data_strcasestr(n_str, "Gauntlets")) {
                t->category = ITEM_HANDS;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (data_strcasestr(n_str, "Bracers")) {
                t->category = ITEM_BRACELET;
                t->max_durability = 80;
                t->weight = 1.0f;
            } else if (data_strcasestr(n_str, "Bracelet") || data_strcasestr(n_str, "Bangle")) {
                t->category = ITEM_BRACELET;
                t->max_durability = 60;
                t->weight = 0.5f;
            } else if (data_strcasestr(n_str, "Boots") || data_strcasestr(n_str, "Slippers") ||
                       data_strcasestr(n_str, "Shoes")) {
                t->category = ITEM_FEET;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (data_strcasestr(n_str, "Ring")) {
                t->category = ITEM_RING;
                t->weight = 0.1f;
            } else {
                t->category = ITEM_MISC;
            }
        }
    }
}

static void item_category(ItemTemplate *t, const ItemJson *j) {
    const char *cat_str = j->category->valuestring;
    if (strcmp(cat_str, "WEAPON") == 0) {
        item_weapon(t, j);
    } else if (strcmp(cat_str, "ARMOR") == 0) {
        item_armor(t, j);
    } else if (strcmp(cat_str, "POTIONS_OILS") == 0) {
        item_potion(t, j);
    } else if (strcmp(cat_str, "ADVENTURING_GEAR") == 0) {
        item_gear(t, j);
    } else if (strcmp(cat_str, "BOOK") == 0) {
        item_book(t, j);
    } else if (strcmp(cat_str, "WONDROUS_ITEMS") == 0) {
        item_wondrous(t, j);
    } else {
        t->category = ITEM_MISC;
    }
}

static void item_defaults(ItemTemplate *t) {
    t->category = ITEM_MISC;
    t->str_bonus = 0;
    t->dex_bonus = 0;
    t->con_bonus = 0;
    t->int_bonus = 0;
    t->wis_bonus = 0;
    t->cha_bonus = 0;
    t->damage_dice_count = 0;
    t->damage_dice_sides = 0;
    t->attack_bonus = 0;
    t->ac_bonus = 0;
    t->ac_base = 0;
    t->max_durability = 0;
    t->max_stack = 1;
    t->needs_id = false;
    t->heal_amount = 0;
    t->light_radius = 0;
    t->duration_turns = 0;
    t->cost = 0;
    t->weight = 1.0f;
}

/*"ac": "14 + Dex modifier" is the base of an armor, "+2" the bonus of a
 * shield or of anything else that writes a plus.*/
static void item_ac(ItemTemplate *t, const cJSON *ac) {
    if (!cJSON_IsString(ac)) {
        return;
    }
    const char *ac_str = ac->valuestring;
    int ac_val = 0;
    for (const char *p = ac_str; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            ac_val = str_to_int(p);
            break;
        }
    }
    if (t->category == ITEM_ARMOR) {
        t->ac_base = ac_val;
    } else if (t->category == ITEM_SHIELD || strchr(ac_str, '+')) {
        t->ac_bonus = ac_val;
    }
}

/*The bonus an item's name promises: "... of Strength" gives +2 STR.*/
static void item_name_bonuses(ItemTemplate *t, const char *name) {
    const struct {
        const char *word;
        int *bonus;
        int value;
    } k_words[] = {
        {"Strength", &t->str_bonus, 2},  {"Dexterity", &t->dex_bonus, 2},
        {"Speed", &t->dex_bonus, 2},     {"Constitution", &t->con_bonus, 2},
        {"Health", &t->con_bonus, 2},    {"Intelligence", &t->int_bonus, 2},
        {"Wisdom", &t->wis_bonus, 2},    {"Charisma", &t->cha_bonus, 2},
        {"Protection", &t->ac_bonus, 1},
    };
    for (size_t i = 0; i < sizeof(k_words) / sizeof(k_words[0]); i++) {
        if (data_strcasestr(name, k_words[i].word)) {
            *k_words[i].bonus = k_words[i].value;
        }
    }
}

/*"cost" is in copper pieces (a number or a string): at least 1 gp.*/
static void item_cost(ItemTemplate *t, const cJSON *cost) {
    if (!cost) {
        return;
    }
    double c_val = 0.0;
    if (cJSON_IsString(cost)) {
        if (!parse_double(cost->valuestring, &c_val, NULL)) {
            c_val = 0.0;
        }
    } else if (cJSON_IsNumber(cost)) {
        c_val = cost->valuedouble;
    }
    uint64_t gp_cost = (uint64_t)(c_val / 100.0);
    t->cost = gp_cost < 1 ? 1 : gp_cost;
}

/*One item of items.json.*/
static void parse_item(ItemTemplate *t, cJSON *item) {
    cJSON *name = cJSON_GetObjectItem(item, "name");
    cJSON *desc = cJSON_GetObjectItem(item, "description");
    cJSON *category = cJSON_GetObjectItem(item, "category");
    cJSON *classification = cJSON_GetObjectItem(item, "classification");
    cJSON *ac = cJSON_GetObjectItem(item, "ac");
    cJSON *damage = cJSON_GetObjectItem(item, "damage");
    cJSON *properties = cJSON_GetObjectItem(item, "properties");
    cJSON *cost = cJSON_GetObjectItem(item, "cost");

    if (cJSON_IsString(name)) {
        t->name = strdup(name->valuestring);
    }
    if (cJSON_IsString(desc)) {
        t->description = strdup(desc->valuestring);
    } else {
        t->description = strdup("No description available.");
    }

    //New fields (Umoria improvements)
    cJSON *plural = cJSON_GetObjectItem(item, "plural_name");
    if (cJSON_IsString(plural)) {
        t->plural_name = strdup(plural->valuestring);
    } else {
        t->plural_name = NULL; // Fallback handled by UI
    }

    cJSON *cursed = cJSON_GetObjectItem(item, "is_cursed");
    t->is_cursed = cJSON_IsTrue(cursed);

    t->cast_spell_idx = -1;
    cJSON *spell_name = cJSON_GetObjectItem(item, "cast_spell");
    if (cJSON_IsString(spell_name)) {
        t->cast_spell_idx = get_spell_idx_by_name(spell_name->valuestring);
    }

    cJSON *charges = cJSON_GetObjectItem(item, "max_charges");
    t->max_charges = cJSON_IsNumber(charges) ? charges->valueint : 0;

    cJSON *mat = cJSON_GetObjectItem(item, "material");
    t->material = cJSON_IsString(mat) ? string_to_material_type(mat->valuestring) : MAT_NONE;

    item_defaults(t);
    if (cJSON_IsString(category)) {
        ItemJson j = {item, name, desc, category, classification, ac, damage, properties, cost};
        item_category(t, &j);
    }
    item_ac(t, ac);
    if (cJSON_IsString(damage)) {
        int dice[2] = {0, 0};
        if (scan_ints(damage->valuestring, 'd', 2, dice) == 2) {
            t->damage_dice_count = dice[0];
            t->damage_dice_sides = dice[1];
        }
    }
    if (cJSON_IsString(name)) {
        item_name_bonuses(t, name->valuestring);
    }
    item_cost(t, cost);
}

bool load_items_json(const char *filepath) {
    cJSON *root = parse_json_file(filepath);
    if (!root) {
        return false;
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        fprintf(stderr, "[Data Loader] Error: 'items' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }

    item_database_size = cJSON_GetArraySize(items);
    item_database = calloc((size_t)item_database_size, sizeof(ItemTemplate));

    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        parse_item(&item_database[i], item);
        i++;
    }

    cJSON_Delete(root);
    printf("[Data Loader] Successfully loaded %d items from %s\n", item_database_size, filepath);
    return true;
}
