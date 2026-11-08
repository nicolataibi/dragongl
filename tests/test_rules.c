/*
 * DRAGON GL - Unit tests: dice, modifiers, damage, condition table
 *
 * Pure rules-engine tests (rules.c only — no server, no world).
 * Run via `ctest` (see CMakeLists.txt). A fixed RNG seed keeps the
 * statistical assertions reproducible.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "rules.h"

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

int main(void) {
    srand(42); /*reproducible statistical tests*/

    /*--- dice: always in range ---*/
    for (int i = 0; i < 10000; i++) {
        int d6 = rules_roll_dice(1, 6);
        CHECK(d6 >= 1 && d6 <= 6);
        int d320 = rules_roll_dice(3, 20);
        CHECK(d320 >= 3 && d320 <= 60);
    }

    /*--- d20 with advantage/disadvantage ---*/
    for (int i = 0; i < 1000; i++) {
        CHECK(rules_roll_d20(true, false) >= 1);
        CHECK(rules_roll_d20(true, false) <= 20);
        CHECK(rules_roll_d20(false, true) >= 1);
        CHECK(rules_roll_d20(false, true) <= 20);
    }
    /*advantage beats disadvantage over many rolls (law of large numbers)*/
    int adv = 0, dis = 0, both = 0;
    for (int i = 0; i < 2000; i++) {
        adv += rules_roll_d20(true, false);
        dis += rules_roll_d20(false, true);
        both += rules_roll_d20(true, true);
    }
    CHECK(adv > dis);
    CHECK(both >= dis); /*disadvantage wins when both are set*/

    /*--- ability modifier (DnD floor((score-10)/2)) ---*/
    CHECK(rules_get_modifier(8) == -1);
    CHECK(rules_get_modifier(10) == 0);
    CHECK(rules_get_modifier(12) == 1);
    CHECK(rules_get_modifier(18) == 4);
    CHECK(rules_get_modifier(3) == -4);
    CHECK(rules_get_modifier(20) == 5);

    /*--- damage modifiers ---*/
    CHECK(rules_calculate_damage(7, DMG_MOD_NORMAL) == 7);
    CHECK(rules_calculate_damage(7, DMG_MOD_RESISTANCE) == 3);
    CHECK(rules_calculate_damage(8, DMG_MOD_RESISTANCE) == 4);
    CHECK(rules_calculate_damage(7, DMG_MOD_VULNERABILITY) == 14);
    CHECK(rules_calculate_damage(7, DMG_MOD_IMMUNITY) == 0);

    /*--- condition table: every enum value round-trips ---*/
    for (int c = 0; c < MAX_CONDITIONS; c++) {
        const char *n = condition_to_name((ConditionType)c);
        CHECK(n != NULL && n[0] != '\0');
        CHECK(condition_from_name(n) == (ConditionType)c);
    }
    CHECK(condition_from_name("NotACondition") == MAX_CONDITIONS);
    CHECK(condition_from_name(NULL) == MAX_CONDITIONS);
    /*case-insensitive, like every comparison in the engine*/
    CHECK(condition_from_name("burning") == COND_BURNING);
    CHECK(condition_from_name("BURNING") == COND_BURNING);

    /*--- has_condition_t: value-based semantics ---*/
    ActiveEffect fx[2];
    fx[0].name = condition_to_name(COND_POISONED);
    fx[0].trigger = EVENT_ON_TURN_START;
    fx[0].mod_type = MOD_ADDITIVE;
    fx[0].value = 1;
    fx[0].duration_rounds = 3;
    fx[0].is_persistent = false;
    fx[1].name = condition_to_name(COND_BURNING);
    fx[1].trigger = EVENT_ON_TURN_START;
    fx[1].mod_type = MOD_ADDITIVE;
    fx[1].value = 1;
    fx[1].duration_rounds = 2;
    fx[1].is_persistent = false;

    CHECK(rules_has_condition_t(fx, 2, COND_POISONED));
    CHECK(rules_has_condition_t(fx, 2, COND_BURNING));
    CHECK(!rules_has_condition_t(fx, 2, COND_FROZEN));
    CHECK(!rules_has_condition_t(fx, 0, COND_POISONED));
    CHECK(!rules_has_condition_t(NULL, 0, COND_POISONED));

    /*expired (duration 0, not persistent) does not count...*/
    fx[0].duration_rounds = 0;
    CHECK(!rules_has_condition_t(fx, 2, COND_POISONED));
    /*...but a persistent effect does.*/
    fx[0].is_persistent = true;
    CHECK(rules_has_condition_t(fx, 2, COND_POISONED));

    /*the string form is routed through the same table: same answer*/
    CHECK(rules_has_condition(fx, 2, "Poisoned") ==
          rules_has_condition_t(fx, 2, COND_POISONED));

    /*--- effect decay: one round per call, removed at zero ---*/
    ActiveEffect dec[1];
    dec[0] = fx[0];
    dec[0].duration_rounds = 2;
    dec[0].is_persistent = false;
    int n = 1;
    (void)rules_update_effects(dec, &n);
    CHECK(dec[0].duration_rounds == 1);
    CHECK(n == 1);
    (void)rules_update_effects(dec, &n);
    CHECK(n == 0); /*duration hit 0 => removed on the same tick*/

    /*persistent effects survive update() forever*/
    ActiveEffect per[1];
    per[0] = fx[0];
    per[0].duration_rounds = 1;
    per[0].is_persistent = true;
    n = 1;
    for (int i = 0; i < 10; i++) (void)rules_update_effects(per, &n);
    CHECK(n == 1);
    CHECK(per[0].duration_rounds == 1);

    if (g_failures == 0) {
        printf("test_rules: all passed\n");
        return 0;
    }
    printf("test_rules: %d FAILURES\n", g_failures);
    return 1;
}
