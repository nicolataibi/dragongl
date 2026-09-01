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
MonsterTemplate* bestiary_data = NULL;
int bestiary_size = 0;

ItemTemplate* item_database = NULL;
int item_database_size = 0;

SpellTemplate* spell_database = NULL;
int spell_database_size = 0;

//Case-insensitive helper strstr
static const char* my_strcasestr(const char* haystack, const char* needle) {
    if (!haystack) {
        return NULL;
    }
    if (!needle) {
        return NULL;
    }
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
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
static char* read_file_to_string(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = malloc(length + 1);
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
static DamageType string_to_damage_type(const char* str) {
    if (!str) return MAX_DAMAGE_TYPES;
    if (strcasecmp(str, "slashing") == 0 || strcasecmp(str, "taglio") == 0) return DMG_SLASHING;
    if (strcasecmp(str, "piercing") == 0 || strcasecmp(str, "perforante") == 0) return DMG_PIERCING;
    if (strcasecmp(str, "bludgeoning") == 0 || strcasecmp(str, "contundente") == 0) return DMG_BLUDGEONING;
    if (strcasecmp(str, "poison") == 0 || strcasecmp(str, "veleno") == 0) return DMG_POISON;
    if (strcasecmp(str, "acid") == 0 || strcasecmp(str, "acido") == 0) return DMG_ACID;
    if (strcasecmp(str, "fire") == 0 || strcasecmp(str, "fuoco") == 0) return DMG_FIRE;
    if (strcasecmp(str, "cold") == 0 || strcasecmp(str, "freddo") == 0) return DMG_COLD;
    if (strcasecmp(str, "radiant") == 0 || strcasecmp(str, "radiante") == 0) return DMG_RADIANT;
    if (strcasecmp(str, "necrotic") == 0 || strcasecmp(str, "necrotico") == 0) return DMG_NECROTIC;
    if (strcasecmp(str, "lightning") == 0 || strcasecmp(str, "fulmine") == 0) return DMG_LIGHTNING;
    if (strcasecmp(str, "thunder") == 0 || strcasecmp(str, "tuono") == 0) return DMG_THUNDER;
    if (strcasecmp(str, "force") == 0 || strcasecmp(str, "forza") == 0) return DMG_FORCE;
    if (strcasecmp(str, "psychic") == 0 || strcasecmp(str, "psichico") == 0) return DMG_PSYCHIC;
    return MAX_DAMAGE_TYPES;
}

//Helper for mapping strings to conditions
static ConditionType string_to_condition_type(const char* str) {
    if (!str) return MAX_CONDITIONS;
    if (strcasecmp(str, "blinded") == 0 || strcasecmp(str, "accecato") == 0) return COND_BLINDED;
    if (strcasecmp(str, "charmed") == 0 || strcasecmp(str, "affascinato") == 0) return COND_CHARMED;
    if (strcasecmp(str, "deafened") == 0 || strcasecmp(str, "assordato") == 0) return COND_DEAFENED;
    if (strcasecmp(str, "frightened") == 0 || strcasecmp(str, "spaventato") == 0) return COND_FRIGHTENED;
    if (strcasecmp(str, "grappled") == 0 || strcasecmp(str, "afferrato") == 0) return COND_GRAPPLED;
    if (strcasecmp(str, "incapacitated") == 0 || strcasecmp(str, "incapace") == 0) return COND_INCAPACITATED;
    if (strcasecmp(str, "invisible") == 0 || strcasecmp(str, "invisibile") == 0) return COND_INVISIBLE;
    if (strcasecmp(str, "paralyzed") == 0 || strcasecmp(str, "paralizzato") == 0) return COND_PARALYZED;
    if (strcasecmp(str, "petrified") == 0 || strcasecmp(str, "pietrificato") == 0) return COND_PETRIFIED;
    if (strcasecmp(str, "poisoned") == 0 || strcasecmp(str, "avvelenato") == 0) return COND_POISONED;
    if (strcasecmp(str, "prone") == 0 || strcasecmp(str, "attonito") == 0) return COND_PRONE;
    if (strcasecmp(str, "restrained") == 0 || strcasecmp(str, "trattenuto") == 0) return COND_RESTRAINED;
    if (strcasecmp(str, "stunned") == 0 || strcasecmp(str, "stordito") == 0) return COND_STUNNED;
    if (strcasecmp(str, "unconscious") == 0 || strcasecmp(str, "incosciente") == 0) return COND_UNCONSCIOUS;
    if (strcasecmp(str, "burning") == 0 || strcasecmp(str, "on fire") == 0) return COND_BURNING;
    if (strcasecmp(str, "bleeding") == 0 || strcasecmp(str, "sanguinante") == 0) return COND_BLEEDING;
    if (strcasecmp(str, "petrified") == 0 || strcasecmp(str, "pietrificato") == 0) return COND_PETRIFIED;
    if (strcasecmp(str, "cursed") == 0 || strcasecmp(str, "maledetto") == 0) return COND_CURSED;
    if (strcasecmp(str, "frozen") == 0 || strcasecmp(str, "congelato") == 0) return COND_FROZEN;
    return MAX_CONDITIONS;
}

//Main bestiary parsing function
static bool load_bestiary_json(const char* filepath) {
    char* json_string = read_file_to_string(filepath);
    if (!json_string) {
        fprintf(stderr, "[Data Loader] Error: failed to read %s\n", filepath);
        return false;
    }
    
    cJSON* root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        fprintf(stderr, "[Data Loader] Error parsing JSON before: %s\n", cJSON_GetErrorPtr());
        return false;
    }
    
    cJSON* monsters = cJSON_GetObjectItem(root, "monsters");
    if (!cJSON_IsArray(monsters)) {
        fprintf(stderr, "[Data Loader] Error: 'monsters' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }
    
    bestiary_size = cJSON_GetArraySize(monsters);
    bestiary_data = calloc(bestiary_size, sizeof(MonsterTemplate));
    
    int i = 0;
    cJSON* monster = NULL;
    cJSON_ArrayForEach(monster, monsters) {
        cJSON* name = cJSON_GetObjectItem(monster, "name");
        cJSON* hp = cJSON_GetObjectItem(monster, "hp_avg");
        cJSON* ac = cJSON_GetObjectItem(monster, "ac");
        cJSON* xp = cJSON_GetObjectItem(monster, "xp");
        cJSON* gold = cJSON_GetObjectItem(monster, "gold");
        cJSON* desc = cJSON_GetObjectItem(monster, "description");

        //New data-driven fields (Phase 1)
        cJSON* arch       = cJSON_GetObjectItem(monster, "archetype");
        cJSON* spd        = cJSON_GetObjectItem(monster, "speed");
        cJSON* sight      = cJSON_GetObjectItem(monster, "sight_range");
        cJSON* dmg_dice   = cJSON_GetObjectItem(monster, "damage_dice");
        cJSON* dmg_sides  = cJSON_GetObjectItem(monster, "damage_sides");
        cJSON* fl_min     = cJSON_GetObjectItem(monster, "floor_min");
        cJSON* fl_max     = cJSON_GetObjectItem(monster, "floor_max");
        
        cJSON* d_res = cJSON_GetObjectItem(monster, "damage_resistances");
        cJSON* d_imm = cJSON_GetObjectItem(monster, "damage_immunities");
        cJSON* d_vul = cJSON_GetObjectItem(monster, "damage_vulnerabilities");
        cJSON* c_imm = cJSON_GetObjectItem(monster, "condition_immunities");
        
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
        bestiary_data[i].archetype    = cJSON_IsString(arch)    ? strdup(arch->valuestring) : strdup("melee");
        bestiary_data[i].speed        = cJSON_IsNumber(spd)     ? spd->valueint             : 2;
        bestiary_data[i].sight_range  = cJSON_IsNumber(sight)   ? sight->valueint           : 5;
        bestiary_data[i].damage_dice  = cJSON_IsNumber(dmg_dice)  ? dmg_dice->valueint      : 1;
        bestiary_data[i].damage_sides = cJSON_IsNumber(dmg_sides) ? dmg_sides->valueint     : 6;
        bestiary_data[i].floor_min    = cJSON_IsNumber(fl_min)  ? fl_min->valueint          : 0;
        bestiary_data[i].floor_max    = cJSON_IsNumber(fl_max)  ? fl_max->valueint          : 99;


        // Parsing resistenze ai danni
        if (cJSON_IsArray(d_res)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, d_res) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) bestiary_data[i].damage_resistances[dt] = true;
                }
            }
        }
        // Parse damage immunities
        if (cJSON_IsArray(d_imm)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, d_imm) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) bestiary_data[i].damage_immunities[dt] = true;
                }
            }
        }
        // Parse damage vulnerabilities
        if (cJSON_IsArray(d_vul)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, d_vul) {
                if (cJSON_IsString(item)) {
                    DamageType dt = string_to_damage_type(item->valuestring);
                    if (dt < MAX_DAMAGE_TYPES) bestiary_data[i].damage_vulnerabilities[dt] = true;
                }
            }
        }
        // Parse condition immunities
        if (cJSON_IsArray(c_imm)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, c_imm) {
                if (cJSON_IsString(item)) {
                    ConditionType ct = string_to_condition_type(item->valuestring);
                    if (ct < MAX_CONDITIONS) bestiary_data[i].condition_immunities[ct] = true;
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
    if (!name || !spell_database) return -1;
    for (int i = 0; i < spell_database_size; i++) {
        if (strcasecmp(spell_database[i].name, name) == 0) return i;
    }
    return -1;
}

static MaterialType string_to_material_type(const char* str) {
    if (!str) return MAT_NONE;
    if (strcasecmp(str, "cloth") == 0) return MAT_CLOTH;
    if (strcasecmp(str, "leather") == 0) return MAT_LEATHER;
    if (strcasecmp(str, "wood") == 0) return MAT_WOOD;
    if (strcasecmp(str, "bone") == 0) return MAT_BONE;
    if (strcasecmp(str, "stone") == 0) return MAT_STONE;
    if (strcasecmp(str, "iron") == 0) return MAT_IRON;
    if (strcasecmp(str, "steel") == 0) return MAT_STEEL;
    if (strcasecmp(str, "mithril") == 0) return MAT_MITHRIL;
    if (strcasecmp(str, "glass") == 0) return MAT_GLASS;
    if (strcasecmp(str, "paper") == 0) return MAT_PAPER;
    return MAT_NONE;
}

//Loading objects from JSON
static bool load_items_json(const char* filepath) {
    char* json_string = read_file_to_string(filepath);
    if (!json_string) {
        fprintf(stderr, "[Data Loader] Error: failed to read %s\n", filepath);
        return false;
    }
    
    cJSON* root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        fprintf(stderr, "[Data Loader] Error parsing JSON before: %s\n", cJSON_GetErrorPtr());
        return false;
    }
    
    cJSON* items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        fprintf(stderr, "[Data Loader] Error: 'items' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }
    
    item_database_size = cJSON_GetArraySize(items);
    item_database = calloc(item_database_size, sizeof(ItemTemplate));
    
    int i = 0;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* desc = cJSON_GetObjectItem(item, "description");
        cJSON* category = cJSON_GetObjectItem(item, "category");
        cJSON* classification = cJSON_GetObjectItem(item, "classification");
        cJSON* ac = cJSON_GetObjectItem(item, "ac");
        cJSON* damage = cJSON_GetObjectItem(item, "damage");
        cJSON* properties = cJSON_GetObjectItem(item, "properties");
        cJSON* cost = cJSON_GetObjectItem(item, "cost");
        
        if (cJSON_IsString(name)) {
            item_database[i].name = strdup(name->valuestring);
        }
        if (cJSON_IsString(desc)) {
            item_database[i].description = strdup(desc->valuestring);
        } else {
            item_database[i].description = strdup("No description available.");
        }
        
        //New fields (Umoria improvements)
        cJSON* plural = cJSON_GetObjectItem(item, "plural_name");
        if (cJSON_IsString(plural)) {
            item_database[i].plural_name = strdup(plural->valuestring);
        } else {
            item_database[i].plural_name = NULL; // Fallback handled by UI
        }
        
        cJSON* cursed = cJSON_GetObjectItem(item, "is_cursed");
        item_database[i].is_cursed = cJSON_IsTrue(cursed);
        
        item_database[i].cast_spell_idx = -1;
        cJSON* spell_name = cJSON_GetObjectItem(item, "cast_spell");
        if (cJSON_IsString(spell_name)) {
            item_database[i].cast_spell_idx = get_spell_idx_by_name(spell_name->valuestring);
        }
        
        cJSON* charges = cJSON_GetObjectItem(item, "max_charges");
        item_database[i].max_charges = cJSON_IsNumber(charges) ? charges->valueint : 0;
        
        cJSON* mat = cJSON_GetObjectItem(item, "material");
        item_database[i].material = cJSON_IsString(mat) ? string_to_material_type(mat->valuestring) : MAT_NONE;
        
        // Defaults
        item_database[i].category = ITEM_MISC;
        item_database[i].str_bonus = 0;
        item_database[i].dex_bonus = 0;
        item_database[i].con_bonus = 0;
        item_database[i].int_bonus = 0;
        item_database[i].wis_bonus = 0;
        item_database[i].cha_bonus = 0;
        item_database[i].damage_dice_count = 0;
        item_database[i].damage_dice_sides = 0;
        item_database[i].attack_bonus = 0;
        item_database[i].ac_bonus = 0;
        item_database[i].ac_base = 0;
        item_database[i].max_durability = 0;
        item_database[i].max_stack = 1;
        item_database[i].needs_id = false;
        item_database[i].heal_amount = 0;
        item_database[i].light_radius = 0;
        item_database[i].duration_turns = 0;
        item_database[i].cost = 0;
        item_database[i].weight = 1.0f;
        
        if (cJSON_IsString(category)) {
            const char* cat_str = category->valuestring;
            if (strcmp(cat_str, "WEAPON") == 0) {
                bool is_ammo = false;
                if (name) {
                    if (cJSON_IsString(name)) {
                        if (my_strcasestr(name->valuestring, "arrow")) {
                            is_ammo = true;
                        }
                        if (my_strcasestr(name->valuestring, "bolt")) {
                            is_ammo = true;
                        }
                        if (my_strcasestr(name->valuestring, "bullet")) {
                            is_ammo = true;
                        }
                        if (my_strcasestr(name->valuestring, "needle")) {
                            is_ammo = true;
                        }
                    }
                }
                if (is_ammo) {
                    item_database[i].category = ITEM_AMMO;
                    item_database[i].max_stack = 20;
                    item_database[i].weight = 0.05f;
                } else {
                    item_database[i].category = ITEM_WEAPON;
                    item_database[i].max_durability = 100;
                    item_database[i].weight = 3.0f;
                    if (properties) {
                        if (cJSON_IsString(properties)) {
                            if (my_strcasestr(properties->valuestring, "heavy")) {
                                item_database[i].weight = 6.0f;
                            }
                            if (my_strcasestr(properties->valuestring, "two-handed")) {
                                item_database[i].weight = 6.0f;
                            }
                            if (my_strcasestr(properties->valuestring, "light")) {
                                item_database[i].weight = 2.0f;
                            }
                            if (my_strcasestr(properties->valuestring, "finesse")) {
                                item_database[i].weight = 2.0f;
                            }
                        }
                    }
                }
            } else if (strcmp(cat_str, "ARMOR") == 0) {
                bool is_shield = false;
                if (classification) {
                    if (cJSON_IsString(classification)) {
                        if (strcmp(classification->valuestring, "Shield") == 0) {
                            is_shield = true;
                        }
                    }
                }
                if (name) {
                    if (cJSON_IsString(name)) {
                        if (my_strcasestr(name->valuestring, "shield")) {
                            is_shield = true;
                        }
                    }
                }
                if (is_shield) {
                    item_database[i].category = ITEM_SHIELD;
                    item_database[i].ac_bonus = 2;
                    item_database[i].max_durability = 100;
                    item_database[i].weight = 5.0f;
                } else {
                    item_database[i].category = ITEM_ARMOR;
                    item_database[i].max_durability = 200;
                    item_database[i].weight = 15.0f;
                    if (classification) {
                        if (cJSON_IsString(classification)) {
                            if (my_strcasestr(classification->valuestring, "Heavy")) {
                                item_database[i].weight = 55.0f;
                            }
                            if (my_strcasestr(classification->valuestring, "Medium")) {
                                item_database[i].weight = 20.0f;
                            }
                            if (my_strcasestr(classification->valuestring, "Light")) {
                                item_database[i].weight = 10.0f;
                            }
                        }
                    }
                }
            } else if (strcmp(cat_str, "POTIONS_OILS") == 0) {
                bool is_oil = false;
                if (name) {
                    if (cJSON_IsString(name)) {
                        if (my_strcasestr(name->valuestring, "oil")) {
                            if (!my_strcasestr(name->valuestring, "potion")) {
                                is_oil = true;
                            }
                        }
                    }
                }
                if (is_oil) {
                    item_database[i].category = ITEM_FUEL;
                    item_database[i].duration_turns = 8000;
                    item_database[i].max_durability = 8000;
                    item_database[i].max_stack = 1;
                    item_database[i].weight = 1.0f;
                } else {
                    item_database[i].category = ITEM_CONSUMABLE;
                    item_database[i].max_stack = 5;
                    item_database[i].weight = 1.0f;
                    item_database[i].needs_id = true;
                    if (name) {
                        if (cJSON_IsString(name)) {
                            if (my_strcasestr(name->valuestring, "Healing")) {
                                if (my_strcasestr(name->valuestring, "Greater")) {
                                    item_database[i].heal_amount = 40;
                                } else if (my_strcasestr(name->valuestring, "Superior")) {
                                    item_database[i].heal_amount = 80;
                                } else if (my_strcasestr(name->valuestring, "Supreme")) {
                                    item_database[i].heal_amount = 120;
                                } else {
                                    item_database[i].heal_amount = 20;
                                }
                            }
                        }
                    }
                }
            } else if (strcmp(cat_str, "ADVENTURING_GEAR") == 0) {
                bool is_torch = false;
                bool is_lantern = false;
                bool is_ration = false;
                bool is_oil = false;
                if (name) {
                    if (cJSON_IsString(name)) {
                        if (my_strcasestr(name->valuestring, "torch")) {
                            is_torch = true;
                        }
                        if (my_strcasestr(name->valuestring, "lantern")) {
                            is_lantern = true;
                        }
                        if (my_strcasestr(name->valuestring, "ration")) {
                            is_ration = true;
                        }
                        if (strcmp(name->valuestring, "Oil (flask)") == 0) {
                            is_oil = true;
                        }
                    }
                }
                if (is_torch) {
                    item_database[i].category = ITEM_LIGHT_SOURCE;
                    item_database[i].light_radius = 6;
                    item_database[i].duration_turns = 1000;
                    item_database[i].max_durability = 1000;
                    item_database[i].max_stack = 10;
                    item_database[i].weight = 1.0f;
                } else if (is_lantern) {
                    item_database[i].category = ITEM_LIGHT_SOURCE;
                    item_database[i].light_radius = 12;
                    item_database[i].duration_turns = 8000;
                    item_database[i].max_durability = 8000;
                    item_database[i].max_stack = 1;
                    item_database[i].weight = 2.0f;
                } else if (is_ration) {
                    item_database[i].category = ITEM_CONSUMABLE;
                    item_database[i].heal_amount = 5;
                    item_database[i].max_stack = 5;
                    item_database[i].weight = 1.0f;
                } else if (is_oil) {
                    item_database[i].category = ITEM_FUEL;
                    item_database[i].duration_turns = 8000;
                    item_database[i].max_durability = 8000;
                    item_database[i].max_stack = 10;
                    item_database[i].weight = 1.0f;
                } else {
                    item_database[i].category = ITEM_MISC;
                    item_database[i].weight = 1.0f;
                }
            } else if (strcmp(cat_str, "BOOK") == 0) {
                /*-------------------------------------------------------
                 * Magic Books / Spellbooks / Grimoires
                 * The "properties" field contains "LEVEL_X_Y" where X and Y
                 * are the minimum and maximum levels of the spells contained.
                 * The "book_class_mask" field is the bitmask of the classes
                 * (ClassType bits, see include/classes.h) that can read and
                 * learn spells from this book.
                 * -------------------------------------------------------*/
                item_database[i].category  = ITEM_BOOK;
                item_database[i].weight    = 2.0f;
                item_database[i].max_stack = 1;
                item_database[i].needs_id  = false;

                /* Spell levels from LEVEL_X_Y property (legacy) */
                cJSON *props = cJSON_GetObjectItem(item, "properties");
                if (props && cJSON_IsString(props)) {
                    const char *p = props->valuestring;
                    const char *level_tag = my_strcasestr(p, "LEVEL_");
                    if (level_tag) {
                        int mn = 0;
                        int mx = 0;
                        sscanf(level_tag + 6, "%d_%d", &mn, &mx);
                        item_database[i].book_min_level = mn;
                        item_database[i].book_max_level = mx;
                    }
                }

                /*book_seq: book sequence in the class series (0-5)*/
                cJSON *bseq = cJSON_GetObjectItem(item, "book_seq");
                if (bseq && cJSON_IsNumber(bseq)) {
                    item_database[i].book_seq = (int)bseq->valuedouble;
                } else {
                    item_database[i].book_seq = 0;
                }

                /*spells: Array of spell names sorted by ascending level*/
                item_database[i].book_spell_count = 0;
                cJSON *spells_arr = cJSON_GetObjectItem(item, "spells");
                if (spells_arr && cJSON_IsArray(spells_arr)) {
                    int sc = cJSON_GetArraySize(spells_arr);
                    if (sc > MAX_BOOK_SPELLS) {
                        sc = MAX_BOOK_SPELLS;
                    }
                    for (int si = 0; si < sc; si++) {
                        cJSON *sp_name = cJSON_GetArrayItem(spells_arr, si);
                        if (sp_name && cJSON_IsString(sp_name)) {
                            strncpy(item_database[i].book_spell_names[si],
                                    sp_name->valuestring, 63);
                            item_database[i].book_spell_names[si][63] = '\0';
                            item_database[i].book_spell_count++;
                        }
                    }
                }

                /* Class mask: read exclusively from the "book_class_mask"
                 * JSON field (bitmask of ClassType, see include/classes.h).
                 * Books without a valid mask stay readable by every class. */
                cJSON *mask_field = cJSON_GetObjectItem(item, "book_class_mask");
                if (mask_field && cJSON_IsNumber(mask_field) &&
                    mask_field->valueint > 0) {
                    item_database[i].book_class_mask =
                        (uint32_t)mask_field->valueint;
                } else {
                    item_database[i].book_class_mask = (1u << CLASS_COUNT) - 1;
                }
            } else if (strcmp(cat_str, "WONDROUS_ITEMS") == 0) {

                item_database[i].needs_id = true;
                item_database[i].weight = 1.0f;
                if (name) {
                    if (cJSON_IsString(name)) {
                        const char* n_str = name->valuestring;
                        if (my_strcasestr(n_str, "Helm")) {
                            item_database[i].category = ITEM_HEAD;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Hat")) {
                            item_database[i].category = ITEM_HEAD;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Crown")) {
                            item_database[i].category = ITEM_HEAD;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Amulet")) {
                            item_database[i].category = ITEM_NECK;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Brooch")) {
                            item_database[i].category = ITEM_NECK;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Necklace")) {
                            item_database[i].category = ITEM_NECK;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Periapt")) {
                            item_database[i].category = ITEM_NECK;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Cloak")) {
                            item_database[i].category = ITEM_BACK;
                            item_database[i].weight = 1.0f;
                        } else if (my_strcasestr(n_str, "Cape")) {
                            item_database[i].category = ITEM_BACK;
                            item_database[i].weight = 1.0f;
                        } else if (my_strcasestr(n_str, "Mantle")) {
                            item_database[i].category = ITEM_BACK;
                            item_database[i].weight = 1.0f;
                        } else if (my_strcasestr(n_str, "Gloves")) {
                            item_database[i].category = ITEM_HANDS;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Gauntlets")) {
                            item_database[i].category = ITEM_HANDS;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Bracers")) {
                            item_database[i].category = ITEM_BRACELET;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 1.0f;
                        } else if (my_strcasestr(n_str, "Bracelet")) {
                            item_database[i].category = ITEM_BRACELET;
                            item_database[i].max_durability = 60;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Bangle")) {
                            item_database[i].category = ITEM_BRACELET;
                            item_database[i].max_durability = 60;
                            item_database[i].weight = 0.5f;
                        } else if (my_strcasestr(n_str, "Boots")) {
                            item_database[i].category = ITEM_FEET;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Slippers")) {
                            item_database[i].category = ITEM_FEET;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Shoes")) {
                            item_database[i].category = ITEM_FEET;
                            item_database[i].max_durability = 80;
                            item_database[i].weight = 2.0f;
                        } else if (my_strcasestr(n_str, "Ring")) {
                            item_database[i].category = ITEM_RING;
                            item_database[i].weight = 0.1f;
                        } else {
                            item_database[i].category = ITEM_MISC;
                        }
                    }
                }
            } else {
                item_database[i].category = ITEM_MISC;
            }
        }
        
        if (ac) {
            if (cJSON_IsString(ac)) {
                const char* ac_str = ac->valuestring;
                int ac_val = 0;
                const char* p_ac = ac_str;
                while (*p_ac) {
                    if (isdigit((unsigned char)*p_ac)) {
                        ac_val = atoi(p_ac);
                        break;
                    }
                    p_ac++;
                }
                if (item_database[i].category == ITEM_ARMOR) {
                    item_database[i].ac_base = ac_val;
                } else if (item_database[i].category == ITEM_SHIELD) {
                    item_database[i].ac_bonus = ac_val;
                } else if (strchr(ac_str, '+')) {
                    item_database[i].ac_bonus = ac_val;
                }
            }
        }
        
        if (damage) {
            if (cJSON_IsString(damage)) {
                const char* dmg_str = damage->valuestring;
                int dice_cnt = 0;
                int dice_sds = 0;
                int read_cnt = sscanf(dmg_str, "%dd%d", &dice_cnt, &dice_sds);
                if (read_cnt == 2) {
                    item_database[i].damage_dice_count = dice_cnt;
                    item_database[i].damage_dice_sides = dice_sds;
                }
            }
        }
        
        if (name) {
            if (cJSON_IsString(name)) {
                const char* n_str = name->valuestring;
                if (my_strcasestr(n_str, "Strength")) {
                    item_database[i].str_bonus = 2;
                }
                if (my_strcasestr(n_str, "Forza")) {
                    item_database[i].str_bonus = 2;
                }
                if (my_strcasestr(n_str, "Dexterity")) {
                    item_database[i].dex_bonus = 2;
                }
                if (my_strcasestr(n_str, "Destrezza")) {
                    item_database[i].dex_bonus = 2;
                }
                if (my_strcasestr(n_str, "Speed")) {
                    item_database[i].dex_bonus = 2;
                }
                if (my_strcasestr(n_str, "Speed")) {
                    item_database[i].dex_bonus = 2;
                }
                if (my_strcasestr(n_str, "Constitution")) {
                    item_database[i].con_bonus = 2;
                }
                if (my_strcasestr(n_str, "Salute")) {
                    item_database[i].con_bonus = 2;
                }
                if (my_strcasestr(n_str, "Health")) {
                    item_database[i].con_bonus = 2;
                }
                if (my_strcasestr(n_str, "Intelligence")) {
                    item_database[i].int_bonus = 2;
                }
                if (my_strcasestr(n_str, "Intelligenza")) {
                    item_database[i].int_bonus = 2;
                }
                if (my_strcasestr(n_str, "Wisdom")) {
                    item_database[i].wis_bonus = 2;
                }
                if (my_strcasestr(n_str, "Saggezza")) {
                    item_database[i].wis_bonus = 2;
                }
                if (my_strcasestr(n_str, "Charisma")) {
                    item_database[i].cha_bonus = 2;
                }
                if (my_strcasestr(n_str, "Carisma")) {
                    item_database[i].cha_bonus = 2;
                }
                if (my_strcasestr(n_str, "Protection")) {
                    item_database[i].ac_bonus = 1;
                }
                if (my_strcasestr(n_str, "Protezione")) {
                    item_database[i].ac_bonus = 1;
                }
            }
        }
        
        if (cost) {
            double c_val = 0.0;
            if (cJSON_IsString(cost)) {
                c_val = atof(cost->valuestring);
            } else if (cJSON_IsNumber(cost)) {
                c_val = cost->valuedouble;
            }
            uint64_t gp_cost = (uint64_t)(c_val / 100.0);
            if (gp_cost < 1) {
                gp_cost = 1;
            }
            item_database[i].cost = gp_cost;
        }
        
        i++;
    }
    
    cJSON_Delete(root);
    printf("[Data Loader] Successfully loaded %d items from %s\n", item_database_size, filepath);
    return true;
}

//Loading spells from JSON
static bool load_spells_json(const char* filepath) {
    char* json_string = read_file_to_string(filepath);
    if (!json_string) {
        fprintf(stderr, "[Data Loader] Error: failed to read %s\n", filepath);
        return false;
    }
    
    cJSON* root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        fprintf(stderr, "[Data Loader] Error parsing JSON before: %s\n", cJSON_GetErrorPtr());
        return false;
    }
    
    cJSON* spells = cJSON_GetObjectItem(root, "spells");
    if (!cJSON_IsArray(spells)) {
        fprintf(stderr, "[Data Loader] Error: 'spells' object is not an array in %s\n", filepath);
        cJSON_Delete(root);
        return false;
    }
    
    spell_database_size = cJSON_GetArraySize(spells);
    spell_database = calloc(spell_database_size, sizeof(SpellTemplate));
    
    int i = 0;
    cJSON* spell = NULL;
    cJSON_ArrayForEach(spell, spells) {
        cJSON* name  = cJSON_GetObjectItem(spell, "name");
        cJSON* level = cJSON_GetObjectItem(spell, "level");
        cJSON* range = cJSON_GetObjectItem(spell, "range");
        cJSON* desc  = cJSON_GetObjectItem(spell, "description");
        cJSON* classes_node = cJSON_GetObjectItem(spell, "classes");
        cJSON* innate_node = cJSON_GetObjectItem(spell, "innate");
        
        if (cJSON_IsString(name)) {
            spell_database[i].name = strdup(name->valuestring);
        }

        //Innate class magic (no book required, always known)
        if (innate_node && cJSON_IsNumber(innate_node) && innate_node->valueint != 0) {
            spell_database[i].innate = true;
        }
        
        // Defaults
        spell_database[i].level = 0;
        spell_database[i].target_type = SPELL_TARGET_SINGLE;
        spell_database[i].effect_type = SPELL_EFFECT_DAMAGE;
        spell_database[i].range = 30;
        spell_database[i].radius = 0;
        spell_database[i].dice_count = 0;
        spell_database[i].dice_sides = 0;
        spell_database[i].has_status_effect = false;
        spell_database[i].innate = false;
        
        if (level) {
            if (cJSON_IsString(level)) {
                if (my_strcasestr(level->valuestring, "cantrip")) {
                    spell_database[i].level = 0;
                } else {
                    spell_database[i].level = atoi(level->valuestring);
                }
            } else if (cJSON_IsNumber(level)) {
                spell_database[i].level = level->valueint;
            }
        }
        
        if (range) {
            if (cJSON_IsString(range)) {
                const char* r_str = range->valuestring;
                if (my_strcasestr(r_str, "Self")) {
                    bool has_aoe = false;
                    if (my_strcasestr(r_str, "cone")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_CONE;
                        has_aoe = true;
                    } else if (my_strcasestr(r_str, "line")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_LINE;
                        has_aoe = true;
                    } else if (my_strcasestr(r_str, "sphere") || my_strcasestr(r_str, "cube") || my_strcasestr(r_str, "radius")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_CIRCLE;
                        has_aoe = true;
                    }
                    if (!has_aoe) {
                        spell_database[i].target_type = SPELL_TARGET_SELF;
                    }
                    spell_database[i].range = 0;
                } else if (my_strcasestr(r_str, "Touch")) {
                    spell_database[i].target_type = SPELL_TARGET_SINGLE;
                    spell_database[i].range = 1;
                } else {
                    int r_val = 30;
                    const char* p_range = r_str;
                    while (*p_range) {
                        if (isdigit((unsigned char)*p_range)) {
                            r_val = atoi(p_range);
                            break;
                        }
                        p_range++;
                    }
                    spell_database[i].range = r_val;

                    if (my_strcasestr(r_str, "cone")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_CONE;
                    } else if (my_strcasestr(r_str, "line")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_LINE;
                    } else if (my_strcasestr(r_str, "sphere") || my_strcasestr(r_str, "cube") || my_strcasestr(r_str, "radius")) {
                        spell_database[i].target_type = SPELL_TARGET_AOE_CIRCLE;
                    } else {
                        spell_database[i].target_type = SPELL_TARGET_SINGLE;
                    }
                }
            }
        }
        
        if (desc) {
            if (cJSON_IsString(desc)) {
                const char* d_str = desc->valuestring;
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
                    spell_database[i].effect_type = SPELL_EFFECT_HEAL;
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
                        spell_database[i].effect_type = SPELL_EFFECT_DAMAGE;
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
                            spell_database[i].effect_type = SPELL_EFFECT_BUFF;
                        } else {
                            spell_database[i].effect_type = SPELL_EFFECT_DEBUFF;
                        }
                    }
                }
                
                const char* p_desc = d_str;
                while (*p_desc) {
                    if (isdigit((unsigned char)*p_desc)) {
                        char* end_ptr = NULL;
                        long c = strtol(p_desc, &end_ptr, 10);
                        if (*end_ptr == 'd' || *end_ptr == 'D') {
                            end_ptr++;
                            if (isdigit((unsigned char)*end_ptr)) {
                                long s = strtol(end_ptr, NULL, 10);
                                spell_database[i].dice_count = (int)c;
                                spell_database[i].dice_sides = (int)s;
                                break;
                            }
                        }
                    }
                    p_desc++;
                }
                
                if (spell_database[i].target_type == SPELL_TARGET_AOE_CIRCLE ||
                    spell_database[i].target_type == SPELL_TARGET_AOE_CONE   ||
                    spell_database[i].target_type == SPELL_TARGET_AOE_LINE   ||
                    spell_database[i].target_type == SPELL_TARGET_AOE_CLOUD) {
                    spell_database[i].radius = 4; //default
                    if (my_strcasestr(d_str, "30-foot") || my_strcasestr(d_str, "30 foot")) spell_database[i].radius = 6;
                    else if (my_strcasestr(d_str, "20-foot") || my_strcasestr(d_str, "20 foot")) spell_database[i].radius = 4;
                    else if (my_strcasestr(d_str, "10-foot") || my_strcasestr(d_str, "10 foot")) spell_database[i].radius = 3;
                    else if (my_strcasestr(d_str, "5-foot")  || my_strcasestr(d_str, "5 foot"))  spell_database[i].radius = 2;
                }

                //Damage type detection from description
                spell_database[i].damage_type = DMG_FORCE; //default
                if (my_strcasestr(d_str, "fire"))        spell_database[i].damage_type = DMG_FIRE;
                else if (my_strcasestr(d_str, "acid"))   spell_database[i].damage_type = DMG_ACID;
                else if (my_strcasestr(d_str, "poison")) spell_database[i].damage_type = DMG_POISON;
                else if (my_strcasestr(d_str, "cold") || my_strcasestr(d_str, "ice")) spell_database[i].damage_type = DMG_COLD;
                else if (my_strcasestr(d_str, "lightning") || my_strcasestr(d_str, "thunder")) spell_database[i].damage_type = DMG_LIGHTNING;
                else if (my_strcasestr(d_str, "necrotic")) spell_database[i].damage_type = DMG_NECROTIC;
                else if (my_strcasestr(d_str, "radiant"))  spell_database[i].damage_type = DMG_RADIANT;
                else if (my_strcasestr(d_str, "psychic"))  spell_database[i].damage_type = DMG_PSYCHIC;
                else if (my_strcasestr(d_str, "slashing")) spell_database[i].damage_type = DMG_SLASHING;
                else if (my_strcasestr(d_str, "piercing")) spell_database[i].damage_type = DMG_PIERCING;
                else if (my_strcasestr(d_str, "bludgeoning")) spell_database[i].damage_type = DMG_BLUDGEONING;

                //Persistent cloud: keywords cloud/fog/mist/gas
                spell_database[i].cloud_rounds = 0;
                if (my_strcasestr(d_str, "cloud") || my_strcasestr(d_str, "fog") ||
                    my_strcasestr(d_str, "mist")  || my_strcasestr(d_str, "gas")) {
                    spell_database[i].target_type  = SPELL_TARGET_AOE_CLOUD;
                    spell_database[i].cloud_rounds = 3;
                }
                
                bool is_status = false;
                if (spell_database[i].effect_type == SPELL_EFFECT_BUFF) {
                    is_status = true;
                }
                if (spell_database[i].effect_type == SPELL_EFFECT_DEBUFF) {
                    is_status = true;
                }
                if (is_status) {
                    spell_database[i].has_status_effect = true;
                    spell_database[i].status_effect.name = spell_database[i].name;
                    spell_database[i].status_effect.trigger = EVENT_ON_TURN_START;
                    spell_database[i].status_effect.duration_rounds = 5;
                    spell_database[i].status_effect.is_persistent = false;
                    if (spell_database[i].effect_type == SPELL_EFFECT_BUFF) {
                        spell_database[i].status_effect.mod_type = MOD_ADDITIVE;
                        spell_database[i].status_effect.value = 2;
                    } else {
                        spell_database[i].status_effect.mod_type = MOD_ADDITIVE;
                        spell_database[i].status_effect.value = -2;
                    }
                }
            }
        }

        cJSON* vfx = cJSON_GetObjectItem(spell, "vfx");
        if (vfx && cJSON_IsObject(vfx)) {
            cJSON* vfx_type = cJSON_GetObjectItem(vfx, "type");
            cJSON* vfx_r = cJSON_GetObjectItem(vfx, "r");
            cJSON* vfx_g = cJSON_GetObjectItem(vfx, "g");
            cJSON* vfx_b = cJSON_GetObjectItem(vfx, "b");

            if (cJSON_IsNumber(vfx_type)) spell_database[i].vfx_type = vfx_type->valueint;
            if (cJSON_IsNumber(vfx_r)) spell_database[i].vfx_r = vfx_r->valuedouble;
            if (cJSON_IsNumber(vfx_g)) spell_database[i].vfx_g = vfx_g->valuedouble;
            if (cJSON_IsNumber(vfx_b)) spell_database[i].vfx_b = vfx_b->valuedouble;
        } else {
            /* Default generic VFX */
            spell_database[i].vfx_type = 0;
            spell_database[i].vfx_r = 0.8f;
            spell_database[i].vfx_g = 0.8f;
            spell_database[i].vfx_b = 0.8f;
        }

        /*-------------------------------------------------------
         * Parsing the "classes" field to construct class_mask.
         * The JSON contains a comma-separated string:
         * "classes": "sorcerer,wizard"
         * It is mapped to a uint32_t bitmask where bit N
         * matches the N value of ClassType (from classes.h).
         * -------------------------------------------------------*/
        spell_database[i].class_mask = 0;
        if (classes_node && cJSON_IsString(classes_node)) {
            /*Class_name → ClassType mapping table*/
            static const struct { const char *name; ClassType ct; } cls_map[] = {
                { "barbarian", CLASS_BARBARIAN },
                { "bard",      CLASS_BARD      },
                { "cleric",    CLASS_CLERIC    },
                { "druid",     CLASS_DRUID     },
                { "fighter",   CLASS_FIGHTER   },
                { "monk",      CLASS_MONK      },
                { "paladin",   CLASS_PALADIN   },
                { "ranger",    CLASS_RANGER    },
                { "rogue",     CLASS_ROGUE     },
                { "sorcerer",  CLASS_SORCERER  },
                { "warlock",   CLASS_WARLOCK   },
                { "wizard",    CLASS_WIZARD    },
            };
            static const int CLS_MAP_LEN = 12;
            /*Copy to not modify the JSON string*/
            char cls_buf[256];
            strncpy(cls_buf, classes_node->valuestring, sizeof(cls_buf) - 1);
            cls_buf[sizeof(cls_buf) - 1] = '\0';
            char *tok = strtok(cls_buf, ",");
            while (tok) {
                /* Trim spazi iniziali */
                while (*tok == ' ') tok++;
                for (int m = 0; m < CLS_MAP_LEN; m++) {
                    if (strcasecmp(tok, cls_map[m].name) == 0) {
                        spell_database[i].class_mask |= (1u << (int)cls_map[m].ct);
                        break;
                    }
                }
                tok = strtok(NULL, ",");
            }
        }
        
        i++;
    }
    
    cJSON_Delete(root);
    printf("[Data Loader] Successfully loaded %d spells from %s\n", spell_database_size, filepath);
    return true;
}

bool init_data_loaders(const char* data_dir) {
    char filepath[512];
    
    //Load the bestiary
    snprintf(filepath, sizeof(filepath), "%s/bestiary.json", data_dir);
    if (!load_bestiary_json(filepath)) {
        //Failure not blocking for now, but reported
    }
    
    //Load enchantments before items (items can point to them)
    snprintf(filepath, sizeof(filepath), "%s/spells.json", data_dir);
    if (!load_spells_json(filepath)) {
        // Fallimento
    }
    
    //Load items
    snprintf(filepath, sizeof(filepath), "%s/items.json", data_dir);
    if (!load_items_json(filepath)) {
        // Fallimento
    }
    
    return true;
}

void cleanup_data_loaders(void) {
    if (bestiary_data) {
        for (int i = 0; i < bestiary_size; i++) {
            free((void*)bestiary_data[i].name);
            free((void*)bestiary_data[i].description);
            free((void*)bestiary_data[i].archetype);
        }
        free(bestiary_data);
        bestiary_data = NULL;
    }
    bestiary_size = 0;
    
    if (item_database) {
        for (int i = 0; i < item_database_size; i++) {
            free((void*)item_database[i].name);
            free((void*)item_database[i].description);
            if (item_database[i].plural_name) free((void*)item_database[i].plural_name);
        }
        free(item_database);
        item_database = NULL;
    }
    item_database_size = 0;
    
    if (spell_database) {
        for (int i = 0; i < spell_database_size; i++) {
            free((void*)spell_database[i].name);
        }
        free(spell_database);
        spell_database = NULL;
    }
    spell_database_size = 0;
}
