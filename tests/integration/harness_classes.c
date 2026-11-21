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

/* The slots of LORE.md (Starting Slots & Progression Rates): a full
 * caster starts with 4/3/2, a fighter or a monk 3 Ki, a barbarian or a
 * rogue 2, a paladin or a ranger 1; each level adds one of the highest
 * level the class has. A save keeps the slots left, not the maximum. */
int test_class_spell_slots(void) {
    static const struct {
        ClassType cls;
        int level, l1, l2, l3, top_level, top;
    } k_rows[] = {
        {CLASS_WIZARD, 1, 4, 3, 2, 3, 2},     {CLASS_CLERIC, 5, 5, 5, 3, 3, 3},
        {CLASS_WIZARD, 20, 5, 5, 4, 9, 4},    {CLASS_FIGHTER, 1, 3, 0, 0, 1, 3},
        {CLASS_MONK, 16, 5, 3, 3, 6, 1},      {CLASS_ROGUE, 1, 2, 0, 0, 1, 2},
        {CLASS_BARBARIAN, 20, 4, 3, 3, 6, 5}, {CLASS_PALADIN, 1, 1, 0, 0, 1, 1},
        {CLASS_RANGER, 20, 3, 2, 2, 5, 11},
    };
    for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++) {
        int m[MAX_SPELL_LEVEL + 1];
        class_spell_slots(k_rows[i].cls, k_rows[i].level, m);
        int total = 0;
        for (int lv = 1; lv <= MAX_SPELL_LEVEL; lv++) {
            total += m[lv];
            CHECK(lv <= k_rows[i].top_level || m[lv] == 0, "row %zu: slots of L%d", i, lv);
        }
        int start = k_rows[i].cls == CLASS_WIZARD || k_rows[i].cls == CLASS_CLERIC ? 9 : m[1];
        CHECK(m[1] == k_rows[i].l1 && m[2] == k_rows[i].l2 && m[3] == k_rows[i].l3 &&
                  m[k_rows[i].top_level] == k_rows[i].top,
              "row %zu: %d/%d/%d, L%d %d", i, m[1], m[2], m[3], k_rows[i].top_level,
              m[k_rows[i].top_level]);
        CHECK(k_rows[i].level == 1 ? total == start : total > 0, "row %zu: %d slots", i, total);
    }

    hx_reset_world();
    Client *c = hero(0, "oldsave", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->class_id = CLASS_SORCERER;
    c->level = 3;
    for (int lv = 1; lv <= MAX_SPELL_LEVEL; lv++) {
        c->spell_slots_max[lv] = c->spell_slots[lv] = 25 - lv; /* an older save */
    }
    c->spell_slots[2] = 1;
    save_player_data(c);
    static Client loaded;
    memset(&loaded, 0, sizeof(loaded));
    snprintf(loaded.username, sizeof(loaded.username), "%s", c->username);
    char pwd[40];
    snprintf(pwd, sizeof(pwd), "pw-%s", c->username);
    CHECK(load_player_data(&loaded, pwd) == 1, "no reload");
    CHECK(loaded.spell_slots_max[1] == 5 && loaded.spell_slots_max[2] == 4 &&
              loaded.spell_slots_max[3] == 2 && loaded.spell_slots_max[4] == 0,
          "reloaded maximum %d/%d/%d/%d", loaded.spell_slots_max[1], loaded.spell_slots_max[2],
          loaded.spell_slots_max[3], loaded.spell_slots_max[4]);
    CHECK(loaded.spell_slots[1] == 5 && loaded.spell_slots[2] == 1 && loaded.spell_slots[4] == 0,
          "reloaded slots %d/%d/%d", loaded.spell_slots[1], loaded.spell_slots[2],
          loaded.spell_slots[4]);
    return 0;
}
