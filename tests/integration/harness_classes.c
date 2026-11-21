/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The classes as LORE.md describes them (see harness_classes.h).
 */

#include "harness_classes.h"

#include "harness_util.h"

#include "classes.h"
#include "item_effects.h"
#include "player.h"

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

/* The biggest HP gain of a level up from 1 to 20 (CON 10: no modifier). */
static int biggest_gain(Client *c, ClassType cls) {
    c->class_id = cls;
    c->con = 10;
    c->level = 1;
    c->xp = 0;
    int biggest = 0;
    for (int level = 2; level <= 20; level++) {
        int before = c->max_hp;
        c->xp = get_xp_threshold(level);
        check_level_up(c);
        int gain = c->max_hp - before;
        biggest = gain > biggest ? gain : biggest;
    }
    heard(c);
    return biggest;
}

/* A wizard gains 1d6 a level, a barbarian 1d12 (both gained 1d8). */
int test_class_hit_dice(void) {
    hx_reset_world();
    Client *c = hero(0, "student", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int wizard = biggest_gain(c, CLASS_WIZARD);
    CHECK(c->level == 20 && wizard >= 1 && wizard <= 6, "wizard: a gain of %d", wizard);
    hero(0, "student", FLOOR, CX, CY);
    int barbarian = biggest_gain(c, CLASS_BARBARIAN);
    CHECK(barbarian > 8 && barbarian <= 12, "barbarian: at most %d", barbarian);
    return 0;
}

/* A fighter adds his proficiency (+3 at level 5) to STR and CON saves, a
 * wizard to INT and WIS ones; the others the ability alone. */
int test_class_saves(void) {
    hx_reset_world();
    Client *c = hero(0, "saver", FLOOR, CX, CY); /* level 5, every ability 14 */
    CHECK(c != NULL, "no player");
    c->class_id = CLASS_FIGHTER;
    CHECK(player_save_mod(c, ABILITY_STR) == 5 && player_save_mod(c, ABILITY_CON) == 5 &&
              player_save_mod(c, ABILITY_WIS) == 2,
          "fighter: STR %+d CON %+d WIS %+d", player_save_mod(c, ABILITY_STR),
          player_save_mod(c, ABILITY_CON), player_save_mod(c, ABILITY_WIS));
    c->class_id = CLASS_WIZARD;
    c->level = 17; /* +6 */
    CHECK(player_save_mod(c, ABILITY_INT) == 8 && player_save_mod(c, ABILITY_STR) == 2,
          "wizard: INT %+d STR %+d", player_save_mod(c, ABILITY_INT),
          player_save_mod(c, ABILITY_STR));
    return 0;
}
