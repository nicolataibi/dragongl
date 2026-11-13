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
#include "data_internal.h"
#include "bestiary.h"
#include "items.h"
#include "classes.h"
#include "spell_effect.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

//Definition of dynamic global variables
MonsterTemplate *bestiary_data = NULL;
int bestiary_size = 0;

ItemTemplate *item_database = NULL;
int item_database_size = 0;

SpellTemplate *spell_database = NULL;
int spell_database_size = 0;

//Case-insensitive helper strstr
const char *data_strcasestr(const char *haystack, const char *needle) {
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
    if (length < 0) {
        fclose(f);
        return NULL;
    }

    char *buffer = malloc((size_t)length + 1);
    if (buffer) {
        size_t read_bytes = fread(buffer, 1, (size_t)length, f);
        if (read_bytes != (size_t)length) {
            //Ignore line formatting errors
        }
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

/* The parsed document of a JSON file, or NULL after an error message that
 * names the file and, for a syntax error, its line and column. */
cJSON *parse_json_file(const char *filepath) {
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

/*Sets flags[v] for every name of the array that lookup() knows (v < count).*/
static void name_flags(const cJSON *names, bool *flags, int count, int (*lookup)(const char *)) {
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, names) {
        if (cJSON_IsString(item)) {
            int v = lookup(item->valuestring);
            if (v >= 0 && v < count) {
                flags[v] = true;
            }
        }
    }
}

static int damage_type_index(const char *name) {
    return (int)damage_type_from_name(name);
}

static int condition_index(const char *name) {
    return (int)condition_from_name(name);
}

static int int_or(const cJSON *obj, const char *key, int fallback) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

/*One monster of bestiary.json.*/
static void parse_monster(MonsterTemplate *m, const cJSON *monster) {
    const cJSON *name = cJSON_GetObjectItem(monster, "name");
    const cJSON *desc = cJSON_GetObjectItem(monster, "description");
    const cJSON *arch = cJSON_GetObjectItem(monster, "archetype");
    if (cJSON_IsString(name)) {
        m->name = strdup(name->valuestring);
    }
    if (cJSON_IsString(desc)) {
        m->description = strdup(desc->valuestring);
    }
    m->hp_avg = int_or(monster, "hp_avg", 0);
    m->ac = int_or(monster, "ac", 0);
    m->xp = int_or(monster, "xp", 0);
    m->gold = int_or(monster, "gold", 0);
    m->archetype = strdup(cJSON_IsString(arch) ? arch->valuestring : "melee");
    m->speed = int_or(monster, "speed", 2);
    m->sight_range = int_or(monster, "sight_range", 5);
    m->damage_dice = int_or(monster, "damage_dice", 1);
    m->damage_sides = int_or(monster, "damage_sides", 6);
    m->floor_min = int_or(monster, "floor_min", 0);
    m->floor_max = int_or(monster, "floor_max", 99);
    name_flags(cJSON_GetObjectItem(monster, "damage_resistances"), m->damage_resistances,
               MAX_DAMAGE_TYPES, damage_type_index);
    name_flags(cJSON_GetObjectItem(monster, "damage_immunities"), m->damage_immunities,
               MAX_DAMAGE_TYPES, damage_type_index);
    name_flags(cJSON_GetObjectItem(monster, "damage_vulnerabilities"), m->damage_vulnerabilities,
               MAX_DAMAGE_TYPES, damage_type_index);
    name_flags(cJSON_GetObjectItem(monster, "condition_immunities"), m->condition_immunities,
               MAX_CONDITIONS, condition_index);
}

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
    bestiary_data = calloc((size_t)bestiary_size, sizeof(MonsterTemplate));
    int i = 0;
    cJSON *monster = NULL;
    cJSON_ArrayForEach(monster, monsters) {
        parse_monster(&bestiary_data[i++], monster);
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
            /*Skip the leading spaces*/
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

static void spell_vfx(SpellTemplate *sp, const cJSON *vfx) {
    sp->vfx_type = 0;
    sp->vfx_r = 0.8f;
    sp->vfx_g = 0.8f;
    sp->vfx_b = 0.8f;
    if (!cJSON_IsObject(vfx)) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(vfx, "type");
    const cJSON *r = cJSON_GetObjectItem(vfx, "r");
    const cJSON *g = cJSON_GetObjectItem(vfx, "g");
    const cJSON *b = cJSON_GetObjectItem(vfx, "b");
    if (cJSON_IsNumber(type)) {
        sp->vfx_type = type->valueint;
    }
    if (cJSON_IsNumber(r)) {
        sp->vfx_r = (float)r->valuedouble;
    }
    if (cJSON_IsNumber(g)) {
        sp->vfx_g = (float)g->valuedouble;
    }
    if (cJSON_IsNumber(b)) {
        sp->vfx_b = (float)b->valuedouble;
    }
}

/*One spell of spells.json; false (after the message) when its effect is
 * not valid.*/
static bool parse_spell(SpellTemplate *sp, cJSON *spell, const char *filepath) {
    const cJSON *name = cJSON_GetObjectItem(spell, "name");
    const cJSON *level = cJSON_GetObjectItem(spell, "level");
    const cJSON *school = cJSON_GetObjectItem(spell, "school");
    sp->name = strdup(cJSON_IsString(name) ? name->valuestring : "");
    sp->level = 0;
    if (cJSON_IsString(level) && strcmp(level->valuestring, "cantrip") != 0) {
        sp->level = str_to_int(level->valuestring);
    }
    sp->martial = cJSON_IsString(school) && strcmp(school->valuestring, "martial") == 0;
    char err[160];
    if (!spell_effect_parse(&sp->effect, cJSON_GetObjectItem(spell, "effect"), err, sizeof(err))) {
        fprintf(stderr, "[Data Loader] Error: %s: spell '%s': %s\n", filepath, sp->name, err);
        return false;
    }
    spell_vfx(sp, cJSON_GetObjectItem(spell, "vfx"));
    spell_class_mask(sp, cJSON_GetObjectItem(spell, "classes"));
    return true;
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
    if (spell_database_size > MAX_SPELL_DB_SIZE) {
        fprintf(stderr, "[Data Loader] Error: %s: %d spells, at most %d\n", filepath,
                spell_database_size, MAX_SPELL_DB_SIZE);
        spell_database_size = 0;
        cJSON_Delete(root);
        return false;
    }
    spell_database = calloc((size_t)spell_database_size, sizeof(SpellTemplate));

    int i = 0;
    bool ok = true;
    cJSON *spell = NULL;
    cJSON_ArrayForEach(spell, spells) {
        ok = parse_spell(&spell_database[i], spell, filepath) && ok;
        i++;
    }

    cJSON_Delete(root);
    if (!ok) {
        return false;
    }
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
