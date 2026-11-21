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

/*data_item_effects.c - The "worn" and "use" objects of data/items.json:
 * what a magic item does while worn and what a consumable does when used
 * (LORE.md, "Magic items"). The schema (data/schema/items.schema.json)
 * already checked the names; an unknown one is ignored here.*/

#include "data_internal.h"
#include "items.h"
#include "rules.h"
#include "strnum.h"

#include <string.h>

/*"2d4+2", "1d6", "15": count and sides (none for a plain number) and the
 * bonus. False (the dice untouched) when the text is not dice.*/
bool item_parse_dice(const char *s, ItemDice *out) {
    ItemDice d = {0, 0, 0};
    const char *end = NULL;
    int first = 0;
    if (!s || !parse_int(s, &first, &end)) {
        return false;
    }
    if (*end == 'd' || *end == 'D') {
        d.count = first;
        if (!parse_int(end + 1, &d.sides, &end) || d.count < 1 || d.sides < 1) {
            return false;
        }
        if (*end == '+' || *end == '-') {
            if (!parse_int(end, &d.bonus, &end)) {
                return false;
            }
        }
    } else {
        d.bonus = first;
    }
    if (*end != '\0') {
        return false;
    }
    *out = d;
    return true;
}

static int json_int(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static bool json_bool(const cJSON *obj, const char *key) {
    return cJSON_IsTrue(cJSON_GetObjectItem(obj, key));
}

static void json_dice(const cJSON *obj, const char *key, ItemDice *out) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) {
        (void)item_parse_dice(v->valuestring, out);
    } else if (cJSON_IsNumber(v)) {
        out->bonus = v->valueint;
    }
}

/*A damage type by name; MAX_DAMAGE_TYPES when there is none.*/
static DamageType json_damage_type(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? damage_type_from_name(v->valuestring) : MAX_DAMAGE_TYPES;
}

/*A list of damage type names as a mask; "random" sets *random instead.*/
static uint32_t json_damage_mask(const cJSON *obj, const char *key, bool *random) {
    uint32_t mask = 0;
    const cJSON *v = NULL;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(obj, key)) {
        if (!cJSON_IsString(v)) {
            continue;
        }
        DamageType t = damage_type_from_name(v->valuestring);
        if (t < MAX_DAMAGE_TYPES) {
            mask |= 1u << t;
        } else if (random && strcmp(v->valuestring, "random") == 0) {
            *random = true;
        }
    }
    return mask;
}

static uint32_t json_condition_mask(const cJSON *obj, const char *key) {
    uint32_t mask = 0;
    const cJSON *v = NULL;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(obj, key)) {
        if (cJSON_IsString(v)) {
            ConditionType c = condition_from_name(v->valuestring);
            if (c < MAX_CONDITIONS) {
                mask |= 1u << c;
            }
        }
    }
    return mask;
}

/*{"str": 19} into STR..CHA.*/
static void json_abilities(const cJSON *obj, const char *key, int out[6]) {
    const cJSON *v = NULL;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(obj, key)) {
        Ability a = v->string ? ability_from_name(v->string) : ABILITY_NONE;
        if (a >= ABILITY_STR && a < ABILITY_COUNT && cJSON_IsNumber(v)) {
            out[a] = v->valueint;
        }
    }
}

static void parse_worn_flags(ItemWorn *w, const cJSON *o) {
    w->carried = json_bool(o, "carried");
    w->haste = json_bool(o, "haste");
    w->displacement = json_bool(o, "displacement");
    w->see_invisible = json_bool(o, "see_invisible");
    w->spell_save_advantage = json_bool(o, "spell_save_advantage");
    w->gas_save_advantage = json_bool(o, "gas_save_advantage");
    w->feather_fall = json_bool(o, "feather_fall");
    w->evasion = json_bool(o, "evasion");
    w->sustenance = json_bool(o, "sustenance");
    w->crit_immune = json_bool(o, "crit_immune");
    w->on_hit_charge = json_bool(o, "on_hit_charge");
    w->on_crit_temp_hp = json_bool(o, "on_crit_temp_hp");
}

static void parse_worn(ItemWorn *w, const cJSON *o) {
    w->any = true;
    parse_worn_flags(w, o);
    json_abilities(o, "ability_set", w->ability_set);
    json_abilities(o, "ability_add", w->ability_add);
    w->ability_max = json_int(o, "ability_max");
    w->ac = json_int(o, "ac");
    w->ac_unarmored = json_int(o, "ac_unarmored");
    w->unarmored_base = json_int(o, "unarmored_base");
    w->saves = json_int(o, "saves");
    w->attack = json_int(o, "attack");
    w->damage = json_int(o, "damage");
    w->ranged_damage = json_int(o, "ranged_damage");
    w->spell_dc = json_int(o, "spell_dc");
    w->spell_attack = json_int(o, "spell_attack");
    w->darkvision = json_int(o, "darkvision");
    w->resist = json_damage_mask(o, "resist", &w->resist_random);
    w->resist_choices = json_damage_mask(o, "resist_one_of", NULL);
    w->resist_random |= w->resist_choices != 0;
    w->immune = json_damage_mask(o, "immune", NULL);
    w->immune_conditions = json_condition_mask(o, "immune_conditions");
    json_dice(o, "regen", &w->regen);
    w->regen_rounds = json_int(o, "regen_rounds");
    const cJSON *spell = cJSON_GetObjectItem(o, "immune_spell");
    w->immune_spell = cJSON_IsString(spell) ? get_spell_idx_by_name(spell->valuestring) : -1;
    json_dice(o, "on_hit", &w->on_hit);
    w->on_hit_type = json_damage_type(o, "on_hit_type");
    json_dice(o, "on_crit", &w->on_crit);
    w->on_crit_type = json_damage_type(o, "on_crit_type");
}

static void parse_use(ItemUse *u, const cJSON *o) {
    u->any = true;
    json_dice(o, "heal", &u->heal);
    u->temp_hp = json_int(o, "temp_hp");
    u->rounds = json_int(o, "rounds");
    json_dice(o, "rounds_dice", &u->rounds_dice);
    u->attack = json_int(o, "attack");
    u->damage = json_int(o, "damage");
    u->ac = json_int(o, "ac");
    u->saves = json_int(o, "saves");
    u->darkvision = json_int(o, "darkvision");
    u->haste = json_bool(o, "haste");
    u->resist = json_damage_mask(o, "resist", &u->resist_random);
    u->immune_conditions = json_condition_mask(o, "immune_conditions");
    const cJSON *cond = cJSON_GetObjectItem(o, "condition");
    u->condition = cJSON_IsString(cond) ? condition_from_name(cond->valuestring) : MAX_CONDITIONS;
    const cJSON *save = cJSON_GetObjectItem(o, "save");
    u->save_ability = cJSON_IsString(save) ? ability_from_name(save->valuestring) : ABILITY_NONE;
    u->save_dc = json_int(o, "dc");
    json_dice(o, "harm", &u->harm);
    u->harm_type = json_damage_type(o, "harm_type");
    u->cure = json_condition_mask(o, "cure");
    u->exhaustion = json_bool(o, "exhaustion");
    u->weapon = json_bool(o, "weapon");
}

void item_parse_effects(ItemTemplate *t, const cJSON *item) {
    memset(&t->worn, 0, sizeof(t->worn));
    memset(&t->use, 0, sizeof(t->use));
    t->worn.immune_spell = -1;
    t->worn.on_hit_type = MAX_DAMAGE_TYPES;
    t->worn.on_crit_type = MAX_DAMAGE_TYPES;
    t->use.condition = MAX_CONDITIONS;
    t->use.save_ability = ABILITY_NONE;
    t->use.harm_type = MAX_DAMAGE_TYPES;
    const cJSON *worn = cJSON_GetObjectItem(item, "worn");
    if (cJSON_IsObject(worn)) {
        parse_worn(&t->worn, worn);
    }
    const cJSON *use = cJSON_GetObjectItem(item, "use");
    if (cJSON_IsObject(use)) {
        parse_use(&t->use, use);
    }
    const cJSON *dtype = cJSON_GetObjectItem(item, "damage_type");
    t->damage_type =
        cJSON_IsString(dtype) ? damage_type_from_name(dtype->valuestring) : MAX_DAMAGE_TYPES;
    t->at_will = json_bool(item, "at_will");
    t->no_recharge = json_bool(item, "no_recharge");
    t->charge_cost = json_int(item, "charge_cost");
    if (t->charge_cost < 1) {
        t->charge_cost = 1;
    }
}
