/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Unit tests of the spell effect parser (spell_effect.c) and of the
 * resolution of an effect against one target (spell_rules.c). Dice of one
 * side make every amount exact; the d20 is forced.
 */

#include "cJSON.h"
#include "spell_effect.h"
#include "spell_rules.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define EXPECT(cond, ...)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static bool parse(const char *json, SpellEffect *e, char *err, size_t err_len) {
    cJSON *obj = cJSON_Parse(json);
    bool ok = spell_effect_parse(e, obj, err, err_len);
    cJSON_Delete(obj);
    return ok;
}

static void test_parse_valid(void) {
    SpellEffect e;
    char err[160] = "";
    bool ok = parse("{\"kind\":\"damage\",\"target\":\"circle\",\"range\":30,\"radius\":4,"
                    "\"roll\":\"save\",\"save\":\"dex\",\"on_save\":\"half\","
                    "\"dice\":{\"count\":8,\"sides\":6},\"damage_type\":\"fire\"}",
                    &e, err, sizeof(err));
    EXPECT(ok, "fireball rejected: %s", err);
    EXPECT(e.kind == SPELL_KIND_DAMAGE && e.target == SPELL_TARGET_AOE_CIRCLE, "kind/target");
    EXPECT(e.range == 30 && e.radius == 4 && e.roll == SPELL_ROLL_SAVE, "range/radius/roll");
    EXPECT(e.save == ABILITY_DEX && e.save_halves && e.damage_type == DMG_FIRE, "save/type");
    EXPECT(e.dice_count == 8 && e.dice_sides == 6 && e.dice_add == ABILITY_NONE, "dice");
    EXPECT(e.condition == MAX_CONDITIONS && e.terrain == SPELL_TERRAIN_NONE, "no rider");

    ok = parse("{\"kind\":\"buff\",\"target\":\"self\",\"range\":0,\"rounds\":10,"
               "\"stat\":\"attack\",\"value\":2}",
               &e, err, sizeof(err));
    EXPECT(ok && e.stat == SPELL_BUFF_ATTACK && e.value == 2 && e.rounds == 10, "bless: %s", err);

    ok = parse("{\"kind\":\"special\",\"target\":\"self\",\"range\":0,\"handler\":\"blink\"}", &e,
               err, sizeof(err));
    EXPECT(ok && e.handler == SPELL_HANDLER_BLINK, "blink: %s", err);

    ok = parse("{\"kind\":\"damage\",\"target\":\"single\",\"range\":1,\"roll\":\"auto\","
               "\"save\":\"con\",\"dc\":14,\"dice\":{\"count\":1,\"sides\":10,\"add\":\"dex\"},"
               "\"damage_type\":\"piercing\",\"condition\":\"poisoned\",\"rounds\":3}",
               &e, err, sizeof(err));
    EXPECT(ok && e.dc == 14 && e.dice_add == ABILITY_DEX && e.condition == COND_POISONED,
           "maneuver: %s", err);
}

static void expect_error(const char *json, const char *needle) {
    SpellEffect e;
    char err[160] = "";
    bool ok = parse(json, &e, err, sizeof(err));
    EXPECT(!ok, "accepted: %s", json);
    EXPECT(strstr(err, needle) != NULL, "error '%s' does not name %s", err, needle);
}

static void test_parse_errors(void) {
    expect_error("{\"target\":\"self\",\"range\":0}", "effect.kind");
    expect_error("{\"kind\":\"zap\",\"target\":\"self\",\"range\":0}", "effect.kind");
    expect_error("{\"kind\":\"utility\",\"target\":\"self\",\"range\":61}", "effect.range");
    expect_error("{\"kind\":\"damage\",\"target\":\"circle\",\"range\":0,\"roll\":\"auto\","
                 "\"dice\":{\"count\":1,\"sides\":6},\"damage_type\":\"fire\"}",
                 "effect.radius");
    expect_error("{\"kind\":\"damage\",\"target\":\"single\",\"range\":1,\"roll\":\"save\","
                 "\"dice\":{\"count\":1,\"sides\":6},\"damage_type\":\"fire\"}",
                 "effect.save");
    expect_error("{\"kind\":\"damage\",\"target\":\"single\",\"range\":1,\"roll\":\"save\","
                 "\"save\":\"dex\",\"dice\":{\"count\":1,\"sides\":6},\"damage_type\":\"fire\"}",
                 "effect.on_save");
    expect_error("{\"kind\":\"damage\",\"target\":\"single\",\"range\":1,\"roll\":\"auto\","
                 "\"dice\":{\"count\":0,\"sides\":6},\"damage_type\":\"fire\"}",
                 "effect.dice");
    expect_error("{\"kind\":\"damage\",\"target\":\"single\",\"range\":1,\"roll\":\"auto\","
                 "\"dice\":{\"count\":1,\"sides\":6},\"damage_type\":\"plasma\"}",
                 "effect.damage_type");
    expect_error("{\"kind\":\"condition\",\"target\":\"single\",\"range\":6,\"roll\":\"auto\","
                 "\"condition\":\"sleepy\",\"rounds\":1}",
                 "effect.condition");
    expect_error("{\"kind\":\"condition\",\"target\":\"single\",\"range\":6,\"roll\":\"auto\","
                 "\"condition\":\"prone\"}",
                 "effect.rounds");
    expect_error("{\"kind\":\"special\",\"target\":\"self\",\"range\":0,\"handler\":\"wish\"}",
                 "effect.handler");
    expect_error("{\"kind\":\"buff\",\"target\":\"self\",\"range\":0,\"rounds\":1,"
                 "\"stat\":\"luck\",\"value\":1}",
                 "effect.stat");
}

static SpellCaster caster(int level, int mod) {
    SpellCaster c;
    memset(&c, 0, sizeof(c));
    c.level = level;
    for (int a = 0; a < ABILITY_COUNT; a++) {
        c.mods[a] = mod;
    }
    c.spell_ability = ABILITY_INT;
    return c;
}

static void test_numbers(void) {
    EXPECT(spell_proficiency(1) == 2 && spell_proficiency(5) == 3 && spell_proficiency(17) == 6 &&
               spell_proficiency(20) == 6,
           "proficiency");
    EXPECT(class_spell_ability(CLASS_WIZARD) == ABILITY_INT &&
               class_spell_ability(CLASS_CLERIC) == ABILITY_WIS &&
               class_spell_ability(CLASS_WARLOCK) == ABILITY_CHA &&
               class_spell_ability(CLASS_ROGUE) == ABILITY_DEX &&
               class_spell_ability(CLASS_FIGHTER) == ABILITY_STR,
           "casting abilities");
    SpellCaster c = caster(5, 4);
    SpellEffect e;
    memset(&e, 0, sizeof(e));
    e.dice_count = 1;
    e.dice_sides = 10;
    e.dice_add = ABILITY_NONE;
    EXPECT(spell_save_dc(&e, &c) == 15 && spell_attack_bonus(&c) == 7, "DC 8+3+4, attack +7");
    e.dc = 12;
    EXPECT(spell_save_dc(&e, &c) == 12, "a fixed DC wins");
    e.cantrip_scaling = true;
    EXPECT(spell_dice_count(&e, 4) == 1 && spell_dice_count(&e, 5) == 2 &&
               spell_dice_count(&e, 11) == 3 && spell_dice_count(&e, 17) == 4,
           "cantrip tiers");
    e.dice_bonus = 3;
    e.dice_add = ABILITY_SPELL;
    EXPECT(spell_average_amount(&e, &c) == 2 * 5.5 + 3 + 4, "average %f",
           spell_average_amount(&e, &c));
}

/*A damage effect of exactly 4 (4d1), so every amount is known.*/
static SpellEffect four_damage(SpellRoll roll) {
    SpellEffect e;
    memset(&e, 0, sizeof(e));
    e.kind = SPELL_KIND_DAMAGE;
    e.target = SPELL_TARGET_SINGLE;
    e.roll = roll;
    e.save = roll == SPELL_ROLL_SAVE ? ABILITY_DEX : ABILITY_NONE;
    e.dice_count = 4;
    e.dice_sides = 1;
    e.dice_add = ABILITY_NONE;
    e.damage_type = DMG_FIRE;
    e.condition = MAX_CONDITIONS;
    return e;
}

static void test_resolve_attack(void) {
    SpellCaster c = caster(5, 4); /* attack +7 */
    SpellTargetInfo t = {.ac = 17};
    SpellEffect e = four_damage(SPELL_ROLL_ATTACK);
    rules_force_d20(10);
    SpellOutcome o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && !o.crit && o.damage == 4 && o.roll == 10, "10+7 vs 17 hits for 4");
    rules_force_d20(9);
    o = spell_resolve(&e, &c, &t);
    EXPECT(!o.landed && o.damage == 0, "9+7 vs 17 misses");
    rules_force_d20(20);
    t.ac = 99;
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && o.crit && o.damage == 8, "a natural 20 hits and doubles the dice: %d",
           o.damage);
    rules_force_d20(1);
    t.ac = -99;
    o = spell_resolve(&e, &c, &t);
    EXPECT(!o.landed, "a natural 1 misses");
    rules_force_d20(10);
    t.ac = 10;
    t.damage_mod = DMG_MOD_RESISTANCE;
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.damage == 2, "resistance halves: %d", o.damage);
    t.damage_mod = DMG_MOD_IMMUNITY;
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && o.damage == 0, "immunity: %d", o.damage);
    rules_force_d20(0);
}

static void test_resolve_save(void) {
    SpellCaster c = caster(5, 4); /* DC 15 */
    SpellTargetInfo t = {.ac = 10, .save_bonus = 2};
    SpellEffect e = four_damage(SPELL_ROLL_SAVE);
    e.save_halves = true;
    rules_force_d20(12);
    SpellOutcome o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && !o.saved && o.damage == 4, "12+2 vs 15 fails for all: %d", o.damage);
    rules_force_d20(13);
    o = spell_resolve(&e, &c, &t);
    EXPECT(!o.landed && o.saved && o.damage == 2, "13+2 vs 15 saves for half: %d", o.damage);
    e.save_halves = false;
    rules_force_d20(20);
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.saved && o.damage == 0, "on_save none: %d", o.damage);
    rules_force_d20(0);
}

static void test_resolve_conditions(void) {
    SpellCaster c = caster(5, 4); /* DC 15 */
    SpellTargetInfo t = {.ac = 10, .save_bonus = 0};
    SpellEffect e = four_damage(SPELL_ROLL_ATTACK);
    e.condition = COND_PRONE;
    rules_force_d20(10); /* hit; no rider save: the rider lands with the hit */
    SpellOutcome o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && o.condition, "rider without a save");
    e.save = ABILITY_STR; /* the rider now needs a failed save: 10 vs 15 fails */
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && o.condition && !o.saved, "failed rider save");
    rules_force_d20(15); /* hit, and 15 vs 15 resists the rider */
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && o.damage == 4 && !o.condition && o.saved, "resisted rider");
    t.condition_immune = true;
    rules_force_d20(10);
    o = spell_resolve(&e, &c, &t);
    EXPECT(o.landed && !o.condition, "immune to the rider");

    SpellEffect hold;
    memset(&hold, 0, sizeof(hold));
    hold.kind = SPELL_KIND_CONDITION;
    hold.roll = SPELL_ROLL_SAVE;
    hold.save = ABILITY_WIS;
    hold.dice_add = ABILITY_NONE;
    hold.condition = COND_PARALYZED;
    hold.rounds = 10;
    t.condition_immune = false;
    rules_force_d20(14);
    o = spell_resolve(&hold, &c, &t);
    EXPECT(o.condition && o.damage == 0, "14 vs 15: held");
    rules_force_d20(15);
    o = spell_resolve(&hold, &c, &t);
    EXPECT(!o.condition, "15 vs 15: resisted");
    hold.roll = SPELL_ROLL_AUTO;
    hold.save = ABILITY_NONE;
    rules_force_d20(1);
    o = spell_resolve(&hold, &c, &t);
    EXPECT(o.condition && o.roll == 0, "an automatic condition rolls nothing");
    rules_force_d20(0);
}

int main(void) {
    test_parse_valid();
    test_parse_errors();
    test_numbers();
    test_resolve_attack();
    test_resolve_save();
    test_resolve_conditions();
    if (g_failures) {
        fprintf(stderr, "spell_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("spell_rules: all tests passed\n");
    return 0;
}
