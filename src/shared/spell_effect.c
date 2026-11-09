/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_effect.c - Parser of the "effect" object of data/spells.json.
 */

#include "spell_effect.h"

#include <stdio.h>
#include <string.h>

static const char *const k_kinds[SPELL_KIND_COUNT] = {
    [SPELL_KIND_DAMAGE] = "damage",       [SPELL_KIND_HEAL] = "heal",
    [SPELL_KIND_CONDITION] = "condition", [SPELL_KIND_BUFF] = "buff",
    [SPELL_KIND_UTILITY] = "utility",     [SPELL_KIND_SPECIAL] = "special",
};
static const char *const k_targets[SPELL_TARGET_COUNT] = {
    [SPELL_TARGET_SINGLE] = "single",     [SPELL_TARGET_SELF] = "self",
    [SPELL_TARGET_AOE_CIRCLE] = "circle", [SPELL_TARGET_AOE_CONE] = "cone",
    [SPELL_TARGET_AOE_LINE] = "line",     [SPELL_TARGET_AOE_CLOUD] = "cloud",
};
static const char *const k_rolls[SPELL_ROLL_COUNT] = {
    [SPELL_ROLL_AUTO] = "auto", [SPELL_ROLL_ATTACK] = "attack", [SPELL_ROLL_SAVE] = "save"};
static const char *const k_handlers[SPELL_HANDLER_COUNT] = {
    [SPELL_HANDLER_NONE] = "",
    [SPELL_HANDLER_ANIMATE_DEAD] = "animate_dead",
    [SPELL_HANDLER_BLINK] = "blink",
    [SPELL_HANDLER_CHARM_PERSON] = "charm_person",
    [SPELL_HANDLER_CLASS_TRANSIT] = "class_transit",
    [SPELL_HANDLER_DOMINATE_MONSTER] = "dominate_monster",
    [SPELL_HANDLER_INVISIBILITY] = "invisibility",
    [SPELL_HANDLER_SUMMON_ELEMENTAL] = "summon_elemental",
    [SPELL_HANDLER_TELEPORT] = "teleport",
};
static const char *const k_stats[] = {"", "ac", "attack", "damage"};
static const char *const k_terrains[] = {"", "wall", "ice", "mud"};

/*Index of name in table[0..count), or count.*/
static int lookup(const char *const *table, int count, const char *name) {
    for (int i = 0; name && i < count; i++) {
        if (table[i] && table[i][0] && strcmp(table[i], name) == 0) {
            return i;
        }
    }
    return count;
}

static const char *name_of(const char *const *table, int count, int v) {
    return (v >= 0 && v < count && table[v]) ? table[v] : "unknown";
}

const char *spell_kind_name(SpellKind kind) {
    return name_of(k_kinds, SPELL_KIND_COUNT, (int)kind);
}
SpellKind spell_kind_from_name(const char *name) {
    return (SpellKind)lookup(k_kinds, SPELL_KIND_COUNT, name);
}
const char *spell_target_name(SpellTargetType target) {
    return name_of(k_targets, SPELL_TARGET_COUNT, (int)target);
}
SpellTargetType spell_target_from_name(const char *name) {
    return (SpellTargetType)lookup(k_targets, SPELL_TARGET_COUNT, name);
}
const char *spell_roll_name(SpellRoll roll) {
    return name_of(k_rolls, SPELL_ROLL_COUNT, (int)roll);
}
SpellRoll spell_roll_from_name(const char *name) {
    return (SpellRoll)lookup(k_rolls, SPELL_ROLL_COUNT, name);
}
const char *spell_handler_name(SpellHandler handler) {
    return name_of(k_handlers, SPELL_HANDLER_COUNT, (int)handler);
}
SpellHandler spell_handler_from_name(const char *name) {
    return (SpellHandler)lookup(k_handlers, SPELL_HANDLER_COUNT, name);
}

bool spell_target_is_area(SpellTargetType target) {
    return target == SPELL_TARGET_AOE_CIRCLE || target == SPELL_TARGET_AOE_CONE ||
           target == SPELL_TARGET_AOE_LINE || target == SPELL_TARGET_AOE_CLOUD;
}

/* --- Field readers: each one returns false after writing the error. --- */

typedef struct {
    const cJSON *obj;
    char *err;
    size_t err_len;
} Reader;

static bool fail(Reader *r, const char *field, const char *what) {
    snprintf(r->err, r->err_len, "effect.%s: %s", field, what);
    return false;
}

/*An integer field in [lo, hi]; *out keeps its value when the field is
 * absent and not required.*/
static bool read_int(Reader *r, const char *field, bool required, int lo, int hi, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->obj, field);
    if (!v) {
        return required ? fail(r, field, "missing") : true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)v->valueint) {
        return fail(r, field, "not an integer");
    }
    if (v->valueint < lo || v->valueint > hi) {
        return fail(r, field, "out of range");
    }
    *out = v->valueint;
    return true;
}

/*A string field looked up in a name table; *out is the index (count when
 * absent and not required).*/
static bool read_name(Reader *r, const char *field, bool required, const char *const *table,
                      int count, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->obj, field);
    *out = count;
    if (!v) {
        return required ? fail(r, field, "missing") : true;
    }
    if (!cJSON_IsString(v)) {
        return fail(r, field, "not a string");
    }
    *out = lookup(table, count, v->valuestring);
    return *out < count ? true : fail(r, field, "unknown value");
}

static bool read_ability(Reader *r, const cJSON *obj, const char *field, bool allow_spell,
                         Ability *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, field);
    *out = ABILITY_NONE;
    if (!v) {
        return true;
    }
    Ability a = cJSON_IsString(v) ? ability_from_name(v->valuestring) : ABILITY_NONE;
    if (a == ABILITY_NONE || (a == ABILITY_SPELL && !allow_spell)) {
        return fail(r, field, "not an ability");
    }
    *out = a;
    return true;
}

static bool read_dice(Reader *r, SpellEffect *e, bool required) {
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(r->obj, "dice");
    if (!d) {
        return required ? fail(r, "dice", "missing") : true;
    }
    if (!cJSON_IsObject(d)) {
        return fail(r, "dice", "not an object");
    }
    Reader dr = {d, r->err, r->err_len};
    if (!read_int(&dr, "count", true, 0, 100, &e->dice_count) ||
        !read_int(&dr, "sides", true, 0, 20, &e->dice_sides) ||
        !read_int(&dr, "bonus", false, 0, 999, &e->dice_bonus) ||
        !read_ability(&dr, d, "add", true, &e->dice_add)) {
        return false;
    }
    if ((e->dice_count == 0) != (e->dice_sides == 0)) {
        return fail(r, "dice", "count and sides must be both zero or both positive");
    }
    if (e->dice_count == 0 && e->dice_bonus == 0) {
        return fail(r, "dice", "no dice and no bonus");
    }
    return true;
}

static bool read_condition(Reader *r, SpellEffect *e, bool required) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->obj, "condition");
    e->condition = MAX_CONDITIONS;
    if (!v) {
        return required ? fail(r, "condition", "missing") : true;
    }
    e->condition = cJSON_IsString(v) ? condition_from_name(v->valuestring) : MAX_CONDITIONS;
    return e->condition < MAX_CONDITIONS ? true : fail(r, "condition", "unknown condition");
}

/*The fields of each kind, after the common ones.*/
static bool read_kind_fields(Reader *r, SpellEffect *e) {
    bool hits = e->kind == SPELL_KIND_DAMAGE || e->kind == SPELL_KIND_CONDITION;
    int v = 0;
    if (hits || cJSON_GetObjectItemCaseSensitive(r->obj, "roll")) {
        if (!read_name(r, "roll", hits, k_rolls, SPELL_ROLL_COUNT, &v)) {
            return false;
        }
        e->roll = (SpellRoll)v;
    }
    if (!read_ability(r, r->obj, "save", false, &e->save)) {
        return false;
    }
    if (e->roll == SPELL_ROLL_SAVE && e->save == ABILITY_NONE) {
        return fail(r, "save", "missing for a save roll");
    }
    const cJSON *on_save = cJSON_GetObjectItemCaseSensitive(r->obj, "on_save");
    if (e->kind == SPELL_KIND_DAMAGE && e->roll == SPELL_ROLL_SAVE) {
        if (!cJSON_IsString(on_save) || (strcmp(on_save->valuestring, "half") != 0 &&
                                         strcmp(on_save->valuestring, "none") != 0)) {
            return fail(r, "on_save", "must be \"half\" or \"none\"");
        }
        e->save_halves = strcmp(on_save->valuestring, "half") == 0;
    }
    const cJSON *scaling = cJSON_GetObjectItemCaseSensitive(r->obj, "cantrip_scaling");
    e->cantrip_scaling = cJSON_IsTrue(scaling);
    if (!read_int(r, "dc", false, 5, 30, &e->dc) ||
        !read_dice(r, e, e->kind == SPELL_KIND_DAMAGE || e->kind == SPELL_KIND_HEAL) ||
        !read_condition(r, e, e->kind == SPELL_KIND_CONDITION)) {
        return false;
    }
    if (e->kind == SPELL_KIND_DAMAGE) {
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(r->obj, "damage_type");
        e->damage_type =
            cJSON_IsString(dt) ? damage_type_from_name(dt->valuestring) : MAX_DAMAGE_TYPES;
        if (e->damage_type >= MAX_DAMAGE_TYPES) {
            return fail(r, "damage_type", "missing or unknown");
        }
    }
    bool needs_rounds = e->condition < MAX_CONDITIONS || e->kind == SPELL_KIND_BUFF ||
                        e->target == SPELL_TARGET_AOE_CLOUD;
    if (!read_int(r, "rounds", needs_rounds, 1, 100, &e->rounds)) {
        return false;
    }
    if (e->kind == SPELL_KIND_BUFF) {
        if (!read_name(r, "stat", true, k_stats, 4, &v)) {
            return false;
        }
        e->stat = (SpellBuffStat)v;
        if (!read_int(r, "value", true, 1, 10, &e->value)) {
            return false;
        }
    }
    if (e->kind == SPELL_KIND_SPECIAL) {
        if (!read_name(r, "handler", true, k_handlers, SPELL_HANDLER_COUNT, &v)) {
            return false;
        }
        e->handler = (SpellHandler)v;
    }
    return true;
}

bool spell_effect_parse(SpellEffect *e, const cJSON *obj, char *err, size_t err_len) {
    memset(e, 0, sizeof(*e));
    e->save = ABILITY_NONE;
    e->dice_add = ABILITY_NONE;
    e->condition = MAX_CONDITIONS;
    e->damage_type = DMG_FORCE;
    Reader r = {obj, err, err_len};
    if (!cJSON_IsObject(obj)) {
        snprintf(err, err_len, "effect: missing or not an object");
        return false;
    }
    int v = 0;
    if (!read_name(&r, "kind", true, k_kinds, SPELL_KIND_COUNT, &v)) {
        return false;
    }
    e->kind = (SpellKind)v;
    if (!read_name(&r, "target", true, k_targets, SPELL_TARGET_COUNT, &v)) {
        return false;
    }
    e->target = (SpellTargetType)v;
    if (!read_int(&r, "range", true, 0, 60, &e->range) ||
        !read_int(&r, "radius", spell_target_is_area(e->target), 1, 30, &e->radius)) {
        return false;
    }
    if (!read_kind_fields(&r, e)) {
        return false;
    }
    if (!read_name(&r, "terrain", false, k_terrains, 4, &v)) {
        return false;
    }
    e->terrain = v < 4 ? (SpellTerrain)v : SPELL_TERRAIN_NONE;
    return true;
}
