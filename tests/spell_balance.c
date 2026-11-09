/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Spell balance check.
 *
 *   spell_balance <data dir> <limits.json> [--report]
 *
 * Loads data/spells.json with the game's own loader and simulates every
 * damage spell with the game's own resolution (spell_rules.c): `trials`
 * casts by the reference caster of the spell's level against the
 * reference target of that level. The expected damage per target (per
 * cast; for a cloud, over its whole lifetime) must stay within the limits
 * of tests/spell_balance.json, and so must heals, buffs and conditions.
 * --report prints the table of every spell, sorted by level and damage.
 *
 * Reference caster of spell level L (a full caster gets level L at
 * character level 2L-1): level max(1, 2L-1), casting modifier +3, +4 from
 * level 4, +5 from level 8. Cantrips are measured at character level 17.
 * Reference target of level L: AC 13 + L/3, saving throws +1 + L/2.
 */

#include "cJSON.h"
#include "data_loader.h"
#include "spell_effect.h"
#include "spell_rules.h"
#include "spells.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVELS 10

typedef struct {
    int trials;
    unsigned seed;
    double damage_max[LEVELS]; /*expected damage per target, by spell level*/
    double damage_min[LEVELS];
    double heal_max[LEVELS];
    int buff_max[4];        /*by SpellBuffStat*/
    int control_rounds_max; /*rounds of a condition that takes the turn, without a save*/
} Limits;

static bool read_array(const cJSON *root, const char *key, double *out, int n) {
    const cJSON *a = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) {
        fprintf(stderr, "limits: '%s' must be an array of %d numbers\n", key, n);
        return false;
    }
    for (int i = 0; i < n; i++) {
        out[i] = cJSON_GetArrayItem(a, i)->valuedouble;
    }
    return true;
}

static bool read_limits(const char *path, Limits *l) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    static char text[1 << 16];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        fprintf(stderr, "%s: not valid JSON\n", path);
        return false;
    }
    bool ok = read_array(root, "damage_max", l->damage_max, LEVELS) &&
              read_array(root, "damage_min", l->damage_min, LEVELS) &&
              read_array(root, "heal_max", l->heal_max, LEVELS);
    const cJSON *buff = cJSON_GetObjectItem(root, "buff_max");
    l->trials =
        cJSON_GetObjectItem(root, "trials") ? cJSON_GetObjectItem(root, "trials")->valueint : 0;
    l->seed = (unsigned)(cJSON_GetObjectItem(root, "seed")
                             ? cJSON_GetObjectItem(root, "seed")->valuedouble
                             : 1);
    const cJSON *ctl = cJSON_GetObjectItem(root, "control_rounds_max");
    l->control_rounds_max = cJSON_IsNumber(ctl) ? ctl->valueint : 0;
    bool has_buff = cJSON_IsObject(buff);
    if (ok && has_buff) {
        l->buff_max[SPELL_BUFF_AC] = cJSON_GetObjectItem(buff, "ac")->valueint;
        l->buff_max[SPELL_BUFF_ATTACK] = cJSON_GetObjectItem(buff, "attack")->valueint;
        l->buff_max[SPELL_BUFF_DAMAGE] = cJSON_GetObjectItem(buff, "damage")->valueint;
    }
    cJSON_Delete(root);
    if (!ok || l->trials <= 0 || l->control_rounds_max <= 0 || !has_buff) {
        fprintf(stderr,
                "%s: trials, seed, damage_max, damage_min, heal_max, buff_max and "
                "control_rounds_max are required\n",
                path);
        return false;
    }
    return true;
}

static SpellCaster reference_caster(int spell_level) {
    SpellCaster c;
    memset(&c, 0, sizeof(c));
    c.level = spell_level == 0 ? 17 : 2 * spell_level - 1;
    int mod = 3 + (c.level >= 4) + (c.level >= 8);
    for (int a = 0; a < ABILITY_COUNT; a++) {
        c.mods[a] = mod;
    }
    c.spell_ability = ABILITY_INT;
    return c;
}

static SpellTargetInfo reference_target(int spell_level) {
    SpellTargetInfo t;
    memset(&t, 0, sizeof(t));
    t.ac = 13 + spell_level / 3;
    t.save_bonus = 1 + spell_level / 2;
    return t;
}

/*Mean damage per target of one cast (a cloud: of its whole lifetime).*/
static double simulate(const SpellTemplate *sp, int trials) {
    SpellCaster c = reference_caster(sp->level);
    SpellTargetInfo t = reference_target(sp->level);
    double total = 0.0;
    for (int i = 0; i < trials; i++) {
        total += spell_resolve(&sp->effect, &c, &t).damage;
    }
    double mean = total / trials;
    return sp->effect.target == SPELL_TARGET_AOE_CLOUD ? mean * sp->effect.rounds : mean;
}

static bool takes_the_turn(ConditionType c) {
    return c == COND_PARALYZED || c == COND_STUNNED || c == COND_UNCONSCIOUS ||
           c == COND_PETRIFIED || c == COND_INCAPACITATED;
}

/*Every limit of one spell; the number of violations (each printed).*/
static int check_spell(const SpellTemplate *sp, const Limits *l, double dmg) {
    const SpellEffect *e = &sp->effect;
    int bad = 0;
    int lv = sp->level;
    if (e->kind == SPELL_KIND_DAMAGE && (dmg > l->damage_max[lv] || dmg < l->damage_min[lv])) {
        printf("FAIL %s (level %d): expected damage %.1f outside [%.1f, %.1f]\n", sp->name, lv, dmg,
               l->damage_min[lv], l->damage_max[lv]);
        bad++;
    }
    if (e->kind == SPELL_KIND_HEAL) {
        SpellCaster c = reference_caster(lv);
        double heal = spell_average_amount(e, &c);
        if (heal > l->heal_max[lv]) {
            printf("FAIL %s (level %d): heals %.1f, at most %.1f\n", sp->name, lv, heal,
                   l->heal_max[lv]);
            bad++;
        }
    }
    if (e->kind == SPELL_KIND_BUFF && e->value > l->buff_max[e->stat]) {
        printf("FAIL %s: buff +%d, at most +%d\n", sp->name, e->value, l->buff_max[e->stat]);
        bad++;
    }
    bool unresisted = e->roll == SPELL_ROLL_AUTO && e->save == ABILITY_NONE;
    if (e->condition < MAX_CONDITIONS && takes_the_turn(e->condition) && unresisted &&
        e->rounds > l->control_rounds_max) {
        printf("FAIL %s: %s for %d rounds without a save, at most %d\n", sp->name,
               condition_to_name(e->condition), e->rounds, l->control_rounds_max);
        bad++;
    }
    return bad;
}

typedef struct {
    const SpellTemplate *sp;
    double dmg;
} Row;

static int by_level_damage(const void *a, const void *b) {
    const Row *x = a, *y = b;
    if (x->sp->level != y->sp->level) {
        return x->sp->level - y->sp->level;
    }
    return (x->dmg < y->dmg) - (x->dmg > y->dmg);
}

static void report(Row *rows, int n) {
    qsort(rows, (size_t)n, sizeof(Row), by_level_damage);
    printf("%-30s %5s %-7s %-6s %-6s %8s\n", "spell", "level", "target", "roll", "type", "damage");
    for (int i = 0; i < n; i++) {
        const SpellEffect *e = &rows[i].sp->effect;
        printf("%-30s %5d %-7s %-6s %-6.6s %8.1f\n", rows[i].sp->name, rows[i].sp->level,
               spell_target_name(e->target), spell_roll_name(e->roll),
               damage_type_to_name(e->damage_type), rows[i].dmg);
    }
}

static void summary(const Row *rows, int n) {
    printf("level  spells  min     max     mean   (expected damage per target)\n");
    for (int lv = 0; lv < LEVELS; lv++) {
        int count = 0;
        double lo = 1e9, hi = 0.0, sum = 0.0;
        for (int i = 0; i < n; i++) {
            if (rows[i].sp->level == lv) {
                count++;
                sum += rows[i].dmg;
                lo = rows[i].dmg < lo ? rows[i].dmg : lo;
                hi = rows[i].dmg > hi ? rows[i].dmg : hi;
            }
        }
        if (count) {
            printf("%5d  %6d  %-6.1f  %-6.1f  %-6.1f\n", lv, count, lo, hi, sum / count);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <data dir> <limits.json> [--report]\n", argv[0]);
        return 2;
    }
    Limits limits;
    if (!read_limits(argv[2], &limits) || !init_data_loaders(argv[1])) {
        return 2;
    }
    srand(limits.seed);
    static Row rows[MAX_SPELL_DB_SIZE];
    int n = 0, bad = 0;
    for (int i = 0; i < spell_database_size; i++) {
        const SpellTemplate *sp = &spell_database[i];
        double dmg = sp->effect.kind == SPELL_KIND_DAMAGE ? simulate(sp, limits.trials) : 0.0;
        bad += check_spell(sp, &limits, dmg);
        if (sp->effect.kind == SPELL_KIND_DAMAGE) {
            rows[n].sp = sp;
            rows[n++].dmg = dmg;
        }
    }
    summary(rows, n);
    if (argc > 3 && strcmp(argv[3], "--report") == 0) {
        report(rows, n);
    }
    printf("spell_balance: %d damage spells simulated (%d trials each), %d violation(s)\n", n,
           limits.trials, bad);
    return bad ? 1 : 0;
}
