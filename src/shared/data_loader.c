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

#include "strnum.h"
#include "binio.h"
#include "data_loader.h"
#include "cJSON.h"
#include "bestiary.h"
#include "items.h"
#include "spells.h"
#include "classes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

//Definition of dynamic global variables
MonsterTemplate *bestiary_data = NULL;
int bestiary_size = 0;

ItemTemplate *item_database = NULL;
int item_database_size = 0;

SpellTemplate *spell_database = NULL;
int spell_database_size = 0;

//Case-insensitive helper strstr
static const char *my_strcasestr(const char *haystack, const char *needle) {
    if (!haystack) {
        return NULL;
    }
    if (!needle) {
        return NULL;
    }
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h) {
            if (!*n) {
                break;
            }
            int c1 = tolower((unsigned char)*h);
            int c2 = tolower((unsigned char)*n);
            if (c1 != c2) {
                break;
            }
            h++;
            n++;
        }
        if (!*n) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

//Utility for reading a text file into memory
static char *read_file_to_string(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = malloc(length + 1);
    if (buffer) {
        size_t read_bytes = fread(buffer, 1, length, f);
        if (read_bytes != (size_t)length) {
            //Ignore line formatting errors
        }
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

//Helper for mapping strings to damage types
static DamageType string_to_damage_type(const char *str) {
    if (!str) {
        return MAX_DAMAGE_TYPES;
    }
    if (strcasecmp(str, "slashing") == 0 || strcasecmp(str, "taglio") == 0) {
        return DMG_SLASHING;
    }
    if (strcasecmp(str, "piercing") == 0 || strcasecmp(str, "perforante") == 0) {
        return DMG_PIERCING;
    }
    if (strcasecmp(str, "bludgeoning") == 0 || strcasecmp(str, "contundente") == 0) {
        return DMG_BLUDGEONING;
    }
    if (strcasecmp(str, "poison") == 0 || strcasecmp(str, "veleno") == 0) {
        return DMG_POISON;
    }
    if (strcasecmp(str, "acid") == 0 || strcasecmp(str, "acido") == 0) {
        return DMG_ACID;
    }
    if (strcasecmp(str, "fire") == 0 || strcasecmp(str, "fuoco") == 0) {
        return DMG_FIRE;
    }
    if (strcasecmp(str, "cold") == 0 || strcasecmp(str, "freddo") == 0) {
        return DMG_COLD;
    }
    if (strcasecmp(str, "radiant") == 0 || strcasecmp(str, "radiante") == 0) {
        return DMG_RADIANT;
    }
    if (strcasecmp(str, "necrotic") == 0 || strcasecmp(str, "necrotico") == 0) {
        return DMG_NECROTIC;
    }
    if (strcasecmp(str, "lightning") == 0 || strcasecmp(str, "fulmine") == 0) {
        return DMG_LIGHTNING;
    }
    if (strcasecmp(str, "thunder") == 0 || strcasecmp(str, "tuono") == 0) {
        return DMG_THUNDER;
    }
    if (strcasecmp(str, "force") == 0 || strcasecmp(str, "forza") == 0) {
        return DMG_FORCE;
    }
    if (strcasecmp(str, "psychic") == 0 || strcasecmp(str, "psichico") == 0) {
        return DMG_PSYCHIC;
    }
    return MAX_DAMAGE_TYPES;
}

//Helper for mapping strings to conditions
static ConditionType string_to_condition_type(const char *str) {
    if (!str) {
        return MAX_CONDITIONS;
    }
    if (strcasecmp(str, "blinded") == 0 || strcasecmp(str, "accecato") == 0) {
        return COND_BLINDED;
    }
    if (strcasecmp(str, "charmed") == 0 || strcasecmp(str, "affascinato") == 0) {
        return COND_CHARMED;
    }
    if (strcasecmp(str, "deafened") == 0 || strcasecmp(str, "assordato") == 0) {
        return COND_DEAFENED;
    }
    if (strcasecmp(str, "frightened") == 0 || strcasecmp(str, "spaventato") == 0) {
        return COND_FRIGHTENED;
    }
    if (strcasecmp(str, "grappled") == 0 || strcasecmp(str, "afferrato") == 0) {
        return COND_GRAPPLED;
    }
    if (strcasecmp(str, "incapacitated") == 0 || strcasecmp(str, "incapace") == 0) {
        return COND_INCAPACITATED;
    }
    if (strcasecmp(str, "invisible") == 0 || strcasecmp(str, "invisibile") == 0) {
        return COND_INVISIBLE;
    }
    if (strcasecmp(str, "paralyzed") == 0 || strcasecmp(str, "paralizzato") == 0) {
        return COND_PARALYZED;
    }
    if (strcasecmp(str, "petrified") == 0 || strcasecmp(str, "pietrificato") == 0) {
        return COND_PETRIFIED;
    }
    if (strcasecmp(str, "poisoned") == 0 || strcasecmp(str, "avvelenato") == 0) {
        return COND_POISONED;
    }
    if (strcasecmp(str, "prone") == 0 || strcasecmp(str, "attonito") == 0) {
        return COND_PRONE;
    }
    if (strcasecmp(str, "restrained") == 0 || strcasecmp(str, "trattenuto") == 0) {
        return COND_RESTRAINED;
    }
    if (strcasecmp(str, "stunned") == 0 || strcasecmp(str, "stordito") == 0) {
        return COND_STUNNED;
    }
    if (strcasecmp(str, "unconscious") == 0 || strcasecmp(str, "incosciente") == 0) {
        return COND_UNCONSCIOUS;
    }
    if (strcasecmp(str, "burning") == 0 || strcasecmp(str, "on fire") == 0) {
        return COND_BURNING;
    }
    if (strcasecmp(str, "bleeding") == 0 || strcasecmp(str, "sanguinante") == 0) {
        return COND_BLEEDING;
    }
    if (strcasecmp(str, "petrified") == 0 || strcasecmp(str, "pietrificato") == 0) {
        return COND_PETRIFIED;
    }
    if (strcasecmp(str, "cursed") == 0 || strcasecmp(str, "maledetto") == 0) {
        return COND_CURSED;
    }
    if (strcasecmp(str, "frozen") == 0 || strcasecmp(str, "congelato") == 0) {
        return COND_FROZEN;
    }
    return MAX_CONDITIONS;
}

/* The parsed document of a JSON file, or NULL after an error message that
 * names the file and, for a syntax error, its line and column. */
static cJSON *parse_json_file(const char *filepath) {
    char *json_string = read_file_to_string(filepath);
    if (!json_string) {
        fprintf(stderr, "[Data Loader] Error: failed to read %s\n", filepath);
        return NULL;
    }
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        /* The error position points into json_string: use it before free. */
        const char *at = cJSON_GetErrorPtr();
        int line = 1, column = 1;
        for (const char *p = json_string; at && p < at && *p; p++) {
            if (*p == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        fprintf(stderr, "[Data Loader] Error: %s: invalid JSON at line %d, column %d\n", filepath,
                line, column);
    }
    free(json_string);
    return root;
}

//Main bestiary parsing function
static bool load_bestiary_json(const char *filepath) {
    cJSON *root = parse_json_file(filepath);
    if (!root) {
        return false;
    }

    cJSON *monsters = cJSON_GetObjectItem(root, "monsters");
    if (!cJSON_IsArray(monsters)) {
        fprintf(stderr, "[Data Loader] Error: 'monsters' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }

    bestiary_size = cJSON_GetArraySize(monsters);
    bestiary_data = calloc(bestiary_size, sizeof(MonsterTemplate));

    int i = 0;
    cJSON *monster = NULL;
    cJSON_ArrayForEach(monster, monsters) {
        cJSON *name = cJSON_GetObjectItem(monster, "name");
        cJSON *hp = cJSON_GetObjectItem(monster, "hp_avg");
        cJSON *ac = cJSON_GetObjectItem(monster, "ac");
        cJSON *xp = cJSON_GetObjectItem(monster, "xp");
        cJSON *gold = cJSON_GetObjectItem(monster, "gold");
        cJSON *desc = cJSON_GetObjectItem(monster, "description");

        //New data-driven fields (Phase 1)
        cJSON *arch = cJSON_GetObjectItem(monster, "archetype");
        cJSON *spd = cJSON_GetObjectItem(monster, "speed");
        cJSON *sight = cJSON_GetObjectItem(monster, "sight_range");
        cJSON *dmg_dice = cJSON_GetObjectItem(monster, "damage_dice");
        cJSON *dmg_sides = cJSON_GetObjectItem(monster, "damage_sides");
        cJSON *fl_min = cJSON_GetObjectItem(monster, "floor_min");
        cJSON *fl_max = cJSON_GetObjectItem(monster, "floor_max");

        cJSON *d_res = cJSON_GetObjectItem(monster, "damage_resistances");
        cJSON *d_imm = cJSON_GetObjectItem(monster, "damage_immunities");
        cJSON *d_vul = cJSON_GetObjectItem(monster, "damage_vulnerabilities");
        cJSON *c_imm = cJSON_GetObjectItem(monster, "condition_immunities");

        if (cJSON_IsString(name)) {
            bestiary_data[i].name = strdup(name->valuestring);
        }
        if (cJSON_IsNumber(hp)) {
            bestiary_data[i].hp_avg = hp->valueint;
        }
        if (cJSON_IsNumber(ac)) {
            bestiary_data[i].ac = ac->valueint;
        }
        if (cJSON_IsNumber(xp)) {
            bestiary_data[i].xp = xp->valueint;
        }
        if (cJSON_IsNumber(gold)) {
            bestiary_data[i].gold = gold->valueint;
        }
        if (cJSON_IsString(desc)) {
            bestiary_data[i].description = strdup(desc->valuestring);
        }

        //New fields with sensible defaults
        bestiary_data[i].archetype =
            cJSON_IsString(arch) ? strdup(arch->valuestring) : strdup("melee");
        bestiary_data[i].speed = cJSON_IsNumber(spd) ? spd->valueint : 2;
        bestiary_data[i].sight_range = cJSON_IsNumber(sight) ? sight->valueint : 5;
        bestiary_data[i].damage_dice = cJSON_IsNumber(dmg_dice) ? dmg_dice->valueint : 1;
        bestiary_data[i].damage_sides = cJSON_IsNumber(dmg_sides) ? dmg_sides->valueint : 6;
        bestiary_data[i].floor_min = cJSON_IsNumber(fl_min) ? fl_min->valueint : 0;
        bestiary_data[i].floor_max = cJSON_IsNumber(fl_max) ? fl_max->valueint : 99;

        // Parsing resistenze ai danni
        if (cJSON_IsArray(d_res)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, d_res) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) {
                        bestiary_data[i].damage_resistances[dt] = true;
                    }
                }
            }
        }
        // Parse damage immunities
        if (cJSON_IsArray(d_imm)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, d_imm) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) {
                        bestiary_data[i].damage_immunities[dt] = true;
                    }
                }
            }
        }
        // Parse damage vulnerabilities
        if (cJSON_IsArray(d_vul)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, d_vul) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) {
                        bestiary_data[i].damage_vulnerabilities[dt] = true;
                    }
                }
            }
        }
        // Parse condition immunities
        if (cJSON_IsArray(c_imm)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, c_imm) {
                if (cJSON_IsString(item)) {
                    ConditionType ct = string_to_condition_type(item->valuestring);
                    if (ct < MAX_CONDITIONS) {
                        bestiary_data[i].condition_immunities[ct] = true;
                    }
                }
            }
        }

        i++;
    }

    cJSON_Delete(root);
    printf("[Data Loader] Successfully loaded %d monsters from %s\n", bestiary_size, filepath);
    return true;
}

int get_spell_idx_by_name(const char *name) {
    if (!name || !spell_database) {
        return -1;
    }
    for (int i = 0; i < spell_database_size; i++) {
        if (strcasecmp(spell_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

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
            if (my_strcasestr(j->name->valuestring, "arrow")) {
                is_ammo = true;
            }
            if (my_strcasestr(j->name->valuestring, "bolt")) {
                is_ammo = true;
            }
            if (my_strcasestr(j->name->valuestring, "bullet")) {
                is_ammo = true;
            }
            if (my_strcasestr(j->name->valuestring, "needle")) {
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
                if (my_strcasestr(j->properties->valuestring, "heavy")) {
                    t->weight = 6.0f;
                }
                if (my_strcasestr(j->properties->valuestring, "two-handed")) {
                    t->weight = 6.0f;
                }
                if (my_strcasestr(j->properties->valuestring, "light")) {
                    t->weight = 2.0f;
                }
                if (my_strcasestr(j->properties->valuestring, "finesse")) {
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
            if (my_strcasestr(j->name->valuestring, "shield")) {
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
                if (my_strcasestr(j->classification->valuestring, "Heavy")) {
                    t->weight = 55.0f;
                }
                if (my_strcasestr(j->classification->valuestring, "Medium")) {
                    t->weight = 20.0f;
                }
                if (my_strcasestr(j->classification->valuestring, "Light")) {
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
            if (my_strcasestr(j->name->valuestring, "oil")) {
                if (!my_strcasestr(j->name->valuestring, "potion")) {
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
                if (my_strcasestr(j->name->valuestring, "Healing")) {
                    if (my_strcasestr(j->name->valuestring, "Greater")) {
                        t->heal_amount = 40;
                    } else if (my_strcasestr(j->name->valuestring, "Superior")) {
                        t->heal_amount = 80;
                    } else if (my_strcasestr(j->name->valuestring, "Supreme")) {
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
            if (my_strcasestr(j->name->valuestring, "torch")) {
                is_torch = true;
            }
            if (my_strcasestr(j->name->valuestring, "lantern")) {
                is_lantern = true;
            }
            if (my_strcasestr(j->name->valuestring, "ration")) {
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
        const char *level_tag = my_strcasestr(p, "LEVEL_");
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
            if (my_strcasestr(n_str, "Helm") || my_strcasestr(n_str, "Hat") ||
                my_strcasestr(n_str, "Crown")) {
                t->category = ITEM_HEAD;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (my_strcasestr(n_str, "Amulet") || my_strcasestr(n_str, "Brooch") ||
                       my_strcasestr(n_str, "Necklace") || my_strcasestr(n_str, "Periapt")) {
                t->category = ITEM_NECK;
                t->weight = 0.5f;
            } else if (my_strcasestr(n_str, "Cloak") || my_strcasestr(n_str, "Cape") ||
                       my_strcasestr(n_str, "Mantle")) {
                t->category = ITEM_BACK;
                t->weight = 1.0f;
            } else if (my_strcasestr(n_str, "Gloves") || my_strcasestr(n_str, "Gauntlets")) {
                t->category = ITEM_HANDS;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (my_strcasestr(n_str, "Bracers")) {
                t->category = ITEM_BRACELET;
                t->max_durability = 80;
                t->weight = 1.0f;
            } else if (my_strcasestr(n_str, "Bracelet") || my_strcasestr(n_str, "Bangle")) {
                t->category = ITEM_BRACELET;
                t->max_durability = 60;
                t->weight = 0.5f;
            } else if (my_strcasestr(n_str, "Boots") || my_strcasestr(n_str, "Slippers") ||
                       my_strcasestr(n_str, "Shoes")) {
                t->category = ITEM_FEET;
                t->max_durability = 80;
                t->weight = 2.0f;
            } else if (my_strcasestr(n_str, "Ring")) {
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

    // Defaults
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

    if (cJSON_IsString(category)) {
        ItemJson j = {item, name, desc, category, classification, ac, damage, properties, cost};
        item_category(t, &j);
    }

    if (ac) {
        if (cJSON_IsString(ac)) {
            const char *ac_str = ac->valuestring;
            int ac_val = 0;
            const char *p_ac = ac_str;
            while (*p_ac) {
                if (isdigit((unsigned char)*p_ac)) {
                    ac_val = str_to_int(p_ac);
                    break;
                }
                p_ac++;
            }
            if (t->category == ITEM_ARMOR) {
                t->ac_base = ac_val;
            } else if (t->category == ITEM_SHIELD || strchr(ac_str, '+')) {
                t->ac_bonus = ac_val;
            }
        }
    }

    if (damage) {
        if (cJSON_IsString(damage)) {
            const char *dmg_str = damage->valuestring;
            int dice_cnt = 0;
            int dice_sds = 0;
            int dice[2] = {0, 0};
            int read_cnt = scan_ints(dmg_str, 'd', 2, dice);
            dice_cnt = dice[0];
            dice_sds = dice[1];
            if (read_cnt == 2) {
                t->damage_dice_count = dice_cnt;
                t->damage_dice_sides = dice_sds;
            }
        }
    }

    if (name) {
        if (cJSON_IsString(name)) {
            const char *n_str = name->valuestring;
            if (my_strcasestr(n_str, "Strength")) {
                t->str_bonus = 2;
            }
            if (my_strcasestr(n_str, "Forza")) {
                t->str_bonus = 2;
            }
            if (my_strcasestr(n_str, "Dexterity")) {
                t->dex_bonus = 2;
            }
            if (my_strcasestr(n_str, "Destrezza")) {
                t->dex_bonus = 2;
            }
            if (my_strcasestr(n_str, "Speed")) {
                t->dex_bonus = 2;
            }
            if (my_strcasestr(n_str, "Speed")) {
                t->dex_bonus = 2;
            }
            if (my_strcasestr(n_str, "Constitution")) {
                t->con_bonus = 2;
            }
            if (my_strcasestr(n_str, "Salute")) {
                t->con_bonus = 2;
            }
            if (my_strcasestr(n_str, "Health")) {
                t->con_bonus = 2;
            }
            if (my_strcasestr(n_str, "Intelligence")) {
                t->int_bonus = 2;
            }
            if (my_strcasestr(n_str, "Intelligenza")) {
                t->int_bonus = 2;
            }
            if (my_strcasestr(n_str, "Wisdom")) {
                t->wis_bonus = 2;
            }
            if (my_strcasestr(n_str, "Saggezza")) {
                t->wis_bonus = 2;
            }
            if (my_strcasestr(n_str, "Charisma")) {
                t->cha_bonus = 2;
            }
            if (my_strcasestr(n_str, "Carisma")) {
                t->cha_bonus = 2;
            }
            if (my_strcasestr(n_str, "Protection")) {
                t->ac_bonus = 1;
            }
            if (my_strcasestr(n_str, "Protezione")) {
                t->ac_bonus = 1;
            }
        }
    }

    if (cost) {
        double c_val = 0.0;
        if (cJSON_IsString(cost)) {
            if (!parse_double(cost->valuestring, &c_val, NULL)) {
                c_val = 0.0;
            }
        } else if (cJSON_IsNumber(cost)) {
            c_val = cost->valuedouble;
        }
        uint64_t gp_cost = (uint64_t)(c_val / 100.0);
        if (gp_cost < 1) {
            gp_cost = 1;
        }
        t->cost = gp_cost;
    }
}

static bool load_items_json(const char *filepath) {
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
    item_database = calloc(item_database_size, sizeof(ItemTemplate));

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

//Loading spells from JSON
/*"range": "Self", "Touch", "<n> feet" (and its sight/unlimited forms).*/
static void spell_range(SpellTemplate *sp, cJSON *range) {
    if (cJSON_IsString(range)) {
        const char *r_str = range->valuestring;
        if (my_strcasestr(r_str, "Self")) {
            bool has_aoe = false;
            if (my_strcasestr(r_str, "cone")) {
                sp->target_type = SPELL_TARGET_AOE_CONE;
                has_aoe = true;
            } else if (my_strcasestr(r_str, "line")) {
                sp->target_type = SPELL_TARGET_AOE_LINE;
                has_aoe = true;
            } else if (my_strcasestr(r_str, "sphere") || my_strcasestr(r_str, "cube") ||
                       my_strcasestr(r_str, "radius")) {
                sp->target_type = SPELL_TARGET_AOE_CIRCLE;
                has_aoe = true;
            }
            if (!has_aoe) {
                sp->target_type = SPELL_TARGET_SELF;
            }
            sp->range = 0;
        } else if (my_strcasestr(r_str, "Touch")) {
            sp->target_type = SPELL_TARGET_SINGLE;
            sp->range = 1;
        } else {
            int r_val = 30;
            const char *p_range = r_str;
            while (*p_range) {
                if (isdigit((unsigned char)*p_range)) {
                    r_val = str_to_int(p_range);
                    break;
                }
                p_range++;
            }
            sp->range = r_val;

            if (my_strcasestr(r_str, "cone")) {
                sp->target_type = SPELL_TARGET_AOE_CONE;
            } else if (my_strcasestr(r_str, "line")) {
                sp->target_type = SPELL_TARGET_AOE_LINE;
            } else if (my_strcasestr(r_str, "sphere") || my_strcasestr(r_str, "cube") ||
                       my_strcasestr(r_str, "radius")) {
                sp->target_type = SPELL_TARGET_AOE_CIRCLE;
            } else {
                sp->target_type = SPELL_TARGET_SINGLE;
            }
        }
    }
}

/*Target, effect, dice, damage type and condition, read from the English
 * description of the spell.*/
static void spell_from_description(SpellTemplate *sp, cJSON *desc) {
    if (cJSON_IsString(desc)) {
        const char *d_str = desc->valuestring;
        bool is_heal = false;
        if (my_strcasestr(d_str, "regain")) {
            is_heal = true;
        }
        if (my_strcasestr(d_str, "healing")) {
            is_heal = true;
        }
        if (my_strcasestr(d_str, "heals")) {
            is_heal = true;
        }
        if (my_strcasestr(d_str, "restore")) {
            is_heal = true;
        }

        if (is_heal) {
            sp->effect_type = SPELL_EFFECT_HEAL;
        } else {
            bool is_dmg = false;
            if (my_strcasestr(d_str, "damage")) {
                is_dmg = true;
            }
            if (my_strcasestr(d_str, "deals")) {
                is_dmg = true;
            }
            if (my_strcasestr(d_str, "takes")) {
                is_dmg = true;
            }

            if (is_dmg) {
                sp->effect_type = SPELL_EFFECT_DAMAGE;
            } else {
                bool is_buf = false;
                if (my_strcasestr(d_str, "bonus")) {
                    is_buf = true;
                }
                if (my_strcasestr(d_str, "resistance")) {
                    is_buf = true;
                }
                if (my_strcasestr(d_str, "advantage")) {
                    is_buf = true;
                }

                if (is_buf) {
                    sp->effect_type = SPELL_EFFECT_BUFF;
                } else {
                    sp->effect_type = SPELL_EFFECT_DEBUFF;
                }
            }
        }

        const char *p_desc = d_str;
        while (*p_desc) {
            if (isdigit((unsigned char)*p_desc)) {
                char *end_ptr = NULL;
                long c = strtol(p_desc, &end_ptr, 10);
                if (*end_ptr == 'd' || *end_ptr == 'D') {
                    end_ptr++;
                    if (isdigit((unsigned char)*end_ptr)) {
                        long s = strtol(end_ptr, NULL, 10);
                        sp->dice_count = (int)c;
                        sp->dice_sides = (int)s;
                        break;
                    }
                }
            }
            p_desc++;
        }

        if (sp->target_type == SPELL_TARGET_AOE_CIRCLE ||
            sp->target_type == SPELL_TARGET_AOE_CONE || sp->target_type == SPELL_TARGET_AOE_LINE ||
            sp->target_type == SPELL_TARGET_AOE_CLOUD) {
            sp->radius = 4; //default
            if (my_strcasestr(d_str, "30-foot") || my_strcasestr(d_str, "30 foot")) {
                sp->radius = 6;
            } else if (my_strcasestr(d_str, "20-foot") || my_strcasestr(d_str, "20 foot")) {
                sp->radius = 4;
            } else if (my_strcasestr(d_str, "10-foot") || my_strcasestr(d_str, "10 foot")) {
                sp->radius = 3;
            } else if (my_strcasestr(d_str, "5-foot") || my_strcasestr(d_str, "5 foot")) {
                sp->radius = 2;
            }
        }

        //Damage type detection from description
        sp->damage_type = DMG_FORCE; //default
        if (my_strcasestr(d_str, "fire")) {
            sp->damage_type = DMG_FIRE;
        } else if (my_strcasestr(d_str, "acid")) {
            sp->damage_type = DMG_ACID;
        } else if (my_strcasestr(d_str, "poison")) {
            sp->damage_type = DMG_POISON;
        } else if (my_strcasestr(d_str, "cold") || my_strcasestr(d_str, "ice")) {
            sp->damage_type = DMG_COLD;
        } else if (my_strcasestr(d_str, "lightning") || my_strcasestr(d_str, "thunder")) {
            sp->damage_type = DMG_LIGHTNING;
        } else if (my_strcasestr(d_str, "necrotic")) {
            sp->damage_type = DMG_NECROTIC;
        } else if (my_strcasestr(d_str, "radiant")) {
            sp->damage_type = DMG_RADIANT;
        } else if (my_strcasestr(d_str, "psychic")) {
            sp->damage_type = DMG_PSYCHIC;
        } else if (my_strcasestr(d_str, "slashing")) {
            sp->damage_type = DMG_SLASHING;
        } else if (my_strcasestr(d_str, "piercing")) {
            sp->damage_type = DMG_PIERCING;
        } else if (my_strcasestr(d_str, "bludgeoning")) {
            sp->damage_type = DMG_BLUDGEONING;
        }

        //Persistent cloud: keywords cloud/fog/mist/gas
        sp->cloud_rounds = 0;
        if (my_strcasestr(d_str, "cloud") || my_strcasestr(d_str, "fog") ||
            my_strcasestr(d_str, "mist") || my_strcasestr(d_str, "gas")) {
            sp->target_type = SPELL_TARGET_AOE_CLOUD;
            sp->cloud_rounds = 3;
        }

        bool is_status = false;
        if (sp->effect_type == SPELL_EFFECT_BUFF) {
            is_status = true;
        }
        if (sp->effect_type == SPELL_EFFECT_DEBUFF) {
            is_status = true;
        }
        if (is_status) {
            sp->has_status_effect = true;
            sp->status_effect.name = sp->name;
            sp->status_effect.trigger = EVENT_ON_TURN_START;
            sp->status_effect.duration_rounds = 5;
            sp->status_effect.is_persistent = false;
            if (sp->effect_type == SPELL_EFFECT_BUFF) {
                sp->status_effect.mod_type = MOD_ADDITIVE;
                sp->status_effect.value = 2;
            } else {
                sp->status_effect.mod_type = MOD_ADDITIVE;
                sp->status_effect.value = -2;
            }
        }
    }
}

/*"classes": "sorcerer,wizard" -> bit N set for ClassType N (classes.h).*/
static void spell_class_mask(SpellTemplate *sp, cJSON *classes_node) {
    sp->class_mask = 0;
    if (classes_node && cJSON_IsString(classes_node)) {
        /*Class_name → ClassType mapping table*/
        static const struct {
            const char *name;
            ClassType ct;
        } cls_map[] = {
            {"barbarian", CLASS_BARBARIAN}, {"bard", CLASS_BARD},       {"cleric", CLASS_CLERIC},
            {"druid", CLASS_DRUID},         {"fighter", CLASS_FIGHTER}, {"monk", CLASS_MONK},
            {"paladin", CLASS_PALADIN},     {"ranger", CLASS_RANGER},   {"rogue", CLASS_ROGUE},
            {"sorcerer", CLASS_SORCERER},   {"warlock", CLASS_WARLOCK}, {"wizard", CLASS_WIZARD},
        };
        static const int CLS_MAP_LEN = 12;
        /*Copy to not modify the JSON string*/
        char cls_buf[256];
        strncpy(cls_buf, classes_node->valuestring, sizeof(cls_buf) - 1);
        cls_buf[sizeof(cls_buf) - 1] = '\0';
        char *tok = strtok(cls_buf, ",");
        while (tok) {
            /* Trim spazi iniziali */
            while (*tok == ' ') {
                tok++;
            }
            for (int m = 0; m < CLS_MAP_LEN; m++) {
                if (strcasecmp(tok, cls_map[m].name) == 0) {
                    sp->class_mask |= (1u << (int)cls_map[m].ct);
                    break;
                }
            }
            tok = strtok(NULL, ",");
        }
    }
}

/*One spell of spells.json.*/
static void parse_spell(SpellTemplate *sp, cJSON *spell) {
    cJSON *name = cJSON_GetObjectItem(spell, "name");
    cJSON *level = cJSON_GetObjectItem(spell, "level");
    cJSON *range = cJSON_GetObjectItem(spell, "range");
    cJSON *desc = cJSON_GetObjectItem(spell, "description");
    cJSON *classes_node = cJSON_GetObjectItem(spell, "classes");

    if (cJSON_IsString(name)) {
        sp->name = strdup(name->valuestring);
    }

    // Defaults
    sp->level = 0;
    sp->target_type = SPELL_TARGET_SINGLE;
    sp->effect_type = SPELL_EFFECT_DAMAGE;
    sp->range = 30;
    sp->radius = 0;
    sp->dice_count = 0;
    sp->dice_sides = 0;
    sp->has_status_effect = false;

    if (level) {
        if (cJSON_IsString(level)) {
            if (my_strcasestr(level->valuestring, "cantrip")) {
                sp->level = 0;
            } else {
                sp->level = str_to_int(level->valuestring);
            }
        } else if (cJSON_IsNumber(level)) {
            sp->level = level->valueint;
        }
    }

    if (range) {
        spell_range(sp, range);
    }

    if (desc) {
        spell_from_description(sp, desc);
    }

    cJSON *vfx = cJSON_GetObjectItem(spell, "vfx");
    if (vfx && cJSON_IsObject(vfx)) {
        cJSON *vfx_type = cJSON_GetObjectItem(vfx, "type");
        cJSON *vfx_r = cJSON_GetObjectItem(vfx, "r");
        cJSON *vfx_g = cJSON_GetObjectItem(vfx, "g");
        cJSON *vfx_b = cJSON_GetObjectItem(vfx, "b");

        if (cJSON_IsNumber(vfx_type)) {
            sp->vfx_type = vfx_type->valueint;
        }
        if (cJSON_IsNumber(vfx_r)) {
            sp->vfx_r = (float)vfx_r->valuedouble;
        }
        if (cJSON_IsNumber(vfx_g)) {
            sp->vfx_g = (float)vfx_g->valuedouble;
        }
        if (cJSON_IsNumber(vfx_b)) {
            sp->vfx_b = (float)vfx_b->valuedouble;
        }
    } else {
        /* Default generic VFX */
        sp->vfx_type = 0;
        sp->vfx_r = 0.8f;
        sp->vfx_g = 0.8f;
        sp->vfx_b = 0.8f;
    }

    spell_class_mask(sp, classes_node);
}

static bool load_spells_json(const char *filepath) {
    cJSON *root = parse_json_file(filepath);
    if (!root) {
        return false;
    }

    cJSON *spells = cJSON_GetObjectItem(root, "spells");
    if (!cJSON_IsArray(spells)) {
        fprintf(stderr, "[Data Loader] Error: 'spells' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }

    spell_database_size = cJSON_GetArraySize(spells);
    spell_database = calloc(spell_database_size, sizeof(SpellTemplate));

    int i = 0;
    cJSON *spell = NULL;
    cJSON_ArrayForEach(spell, spells) {
        parse_spell(&spell_database[i], spell);
        i++;
    }

    cJSON_Delete(root);
    printf("[Data Loader] Successfully loaded %d spells from %s\n", spell_database_size, filepath);
    return true;
}

/* Computed once per load: the NPC files are written and read many times. */
static uint32_t g_bestiary_fingerprint;

static uint32_t compute_bestiary_fingerprint(void) {
    BinWriter w;
    bw_init(&w);
    bw_u32(&w, (uint32_t)bestiary_size);
    for (int i = 0; i < bestiary_size; i++) {
        const char *name = bestiary_data[i].name ? bestiary_data[i].name : "";
        bw_bytes(&w, name, strlen(name) + 1);
    }
    uint32_t crc = w.failed ? 0 : crc32_data(w.data, w.len);
    bw_free(&w);
    return crc;
}

uint32_t bestiary_fingerprint(void) {
    return g_bestiary_fingerprint;
}

bool init_data_loaders(const char *data_dir) {
    char filepath[512];
    bool ok = true;

    snprintf(filepath, sizeof(filepath), "%s/bestiary.json", data_dir);
    ok = load_bestiary_json(filepath) && ok;
    g_bestiary_fingerprint = compute_bestiary_fingerprint();

    /* Spells before items: an item names the spell it casts. */
    snprintf(filepath, sizeof(filepath), "%s/spells.json", data_dir);
    ok = load_spells_json(filepath) && ok;

    snprintf(filepath, sizeof(filepath), "%s/items.json", data_dir);
    ok = load_items_json(filepath) && ok;

    /* Each loader reported its own error on stderr. */
    return ok;
}

void cleanup_data_loaders(void) {
    g_bestiary_fingerprint = 0;
    if (bestiary_data) {
        for (int i = 0; i < bestiary_size; i++) {
            free((void *)bestiary_data[i].name);
            free((void *)bestiary_data[i].description);
            free((void *)bestiary_data[i].archetype);
        }
        free(bestiary_data);
        bestiary_data = NULL;
    }
    bestiary_size = 0;

    if (item_database) {
        for (int i = 0; i < item_database_size; i++) {
            free((void *)item_database[i].name);
            free((void *)item_database[i].description);
            if (item_database[i].plural_name) {
                free((void *)item_database[i].plural_name);
            }
        }
        free(item_database);
        item_database = NULL;
    }
    item_database_size = 0;

    if (spell_database) {
        for (int i = 0; i < spell_database_size; i++) {
            free((void *)spell_database[i].name);
        }
        free(spell_database);
        spell_database = NULL;
    }
    spell_database_size = 0;
}
