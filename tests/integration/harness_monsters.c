/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Monsters defend and strike as their description says (see
 * harness_monsters.h).
 */

#include "harness_monsters.h"

#include "harness_util.h"

#include "bestiary.h"
#include "inventory.h"
#include "item_effects.h"
#include "items.h"
#include "server_combat.h"
#include "server_internal.h"
#include "spell_cast.h"
#include "spells.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

typedef bool (*TemplateTest)(const MonsterTemplate *m);

/* The first monster of the bestiary that passes the test, as a sturdy
 * neighbour of the hero. */
static NPC *monster_where(int slot, TemplateTest ok) {
    for (int i = 0; i < bestiary_size; i++) {
        if (ok(&bestiary_data[i])) {
            NPC *n = monster(slot, NULL, FLOOR, CX + 1, CY);
            n->template = &bestiary_data[i];
            n->template_idx = i;
            n->hp = n->max_hp = 10000;
            return n;
        }
    }
    fprintf(stderr, "no such monster in the bestiary\n");
    return NULL;
}

static bool resists_slashing(const MonsterTemplate *m) {
    return m->damage_resistances[DMG_SLASHING] && !m->damage_resistances[DMG_BLUDGEONING] &&
           !m->resist_nonmagical && !m->immune_nonmagical;
}
static bool immune_nonmagical(const MonsterTemplate *m) {
    return m->immune_nonmagical && !m->damage_resistances[DMG_SLASHING];
}
static bool immune_fire(const MonsterTemplate *m) {
    return m->damage_immunities[DMG_FIRE];
}
static bool fire_blows(const MonsterTemplate *m) {
    return m->attack_type == DMG_FIRE && m->on_hit >= MAX_CONDITIONS;
}
static bool poisons(const MonsterTemplate *m) {
    return m->on_hit == COND_POISONED;
}
static bool charm_proof(const MonsterTemplate *m) {
    return m->condition_immunities[COND_CHARMED];
}
static bool magic_resistant(const MonsterTemplate *m) {
    return m->magic_resistance && !m->condition_immunities[COND_CHARMED];
}
static bool plain(const MonsterTemplate *m) {
    return !m->magic_resistance && !m->condition_immunities[COND_CHARMED];
}

/* One hit of the hero with what he holds; the damage dealt. */
static int strike(Client *c, NPC *m) {
    int before = m->hp;
    rules_force_d20(15);
    perform_attack(c, m, &g_game);
    rules_force_d20(0);
    heard(c);
    return before - m->hp;
}

/* The blow has the weapon's type: a monster that resists slashing halves
 * a sword, not a mace; one immune to weapons that are not magic takes
 * nothing from a plain sword and full damage from a magic one; an
 * elemental weapon's fire is stopped by an immunity to fire. */
int test_monster_defences(void) {
    hx_reset_world();
    Client *c = hero(0, "slayer", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int sword = item_named("Longsword"), mace = item_named("Mace");
    CHECK(item_database[sword].damage_type == DMG_SLASHING &&
              item_database[mace].damage_type == DMG_BLUDGEONING &&
              item_database[item_named("Vicious Greatsword")].damage_type == DMG_SLASHING &&
              item_database[item_named("Sun Blade")].damage_type == DMG_RADIANT,
          "weapon damage types");

    NPC *m = monster_where(10, resists_slashing);
    CHECK(m != NULL, "no monster resisting slashing");
    c->slot_hand_r = *give(c, sword, 1);
    c->backpack_count = 0;
    strike(c, m);
    CHECK(said("resists your slashing blow"), "slashing not resisted");
    c->slot_hand_r = *give(c, mace, 1);
    c->backpack_count = 0;
    strike(c, m);
    CHECK(!said("resists"), "a mace resisted");

    m = monster_where(11, immune_nonmagical);
    CHECK(m != NULL, "no monster immune to plain weapons");
    c->slot_hand_r = *give(c, sword, 1);
    c->backpack_count = 0;
    CHECK(strike(c, m) == 0 && said("is immune to your slashing blow"), "a plain sword hurt it");
    c->slot_hand_r = *give(c, item_named("Vicious Greatsword"), 1);
    c->backpack_count = 0;
    CHECK(item_is_magic(&c->slot_hand_r) && strike(c, m) > 0, "a magic sword did not");
    c->slot_hand_r = *give(c, mace, 1);
    c->backpack_count = 0;
    c->slot_hand_r.element = ELEM_FLAMING;
    CHECK(item_is_magic(&c->slot_hand_r) && strike(c, m) > 0, "a flaming mace is not magic");

    m = monster_where(12, immune_fire);
    CHECK(m != NULL, "no monster immune to fire");
    rules_force_d20(15);
    int before = m->hp;
    perform_attack(c, m, &g_game);
    rules_force_d20(0);
    heard(c);
    CHECK(before - m->hp >= 1 && !said("is immune to your bludgeoning"),
          "the mace's blow was stopped");
    CHECK(npc_damage_mod(m, DMG_FIRE) == DMG_MOD_IMMUNITY &&
              element_damage_type(ELEM_FLAMING) == DMG_FIRE &&
              element_damage_type(ELEM_NONE) == MAX_DAMAGE_TYPES,
          "fire immunity");
    return 0;
}

/* A monster whose blows burn deals fire damage, which a ring of fire
 * elemental command halves; a venomous one poisons unless the save. */
int test_monster_blows(void) {
    hx_reset_world();
    Client *c = hero(0, "target", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster_where(10, fire_blows);
    CHECK(m != NULL, "no fiery monster");
    m->attack_bonus = 5;
    rules_force_d20(15);
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(said("fire damage") && c->hp < 60, "no fire blow: HP %d", c->hp);
    c->hp = 60;
    give(c, item_named("Ring of Fire Elemental Command"), 1);
    run(c, "wear 1");
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(said("You resist: the damage is halved"), "the ring did not halve the fire");

    hero(0, "target", FLOOR, CX, CY);
    m = monster_where(11, poisons);
    CHECK(m != NULL, "no venomous monster");
    m->attack_bonus = 5; /* DC 13 */
    c->con = 1;          /* 15 - 5 = 10: the save fails */
    perform_attack_npc(m, c, &g_game);
    heard(c);
    CHECK(rules_has_condition_t(c->effects, c->effect_count, COND_POISONED) &&
              said("leaves you Poisoned"),
          "the bite did not poison");
    c->effect_count = 0;
    c->con = 20; /* 15 + 5 = 20 */
    perform_attack_npc(m, c, &g_game);
    rules_force_d20(0);
    heard(c);
    CHECK(c->effect_count == 0 && said("You resist (DC 13)"), "the save did not count");
    return 0;
}

/* Charmed? not a monster immune to it. Magic resistance: advantage on the
 * saving throw against a spell, so more resisted charms. */
static int resisted(Client *c, NPC *m, const SpellTemplate *sp, ConditionType cond, int casts) {
    int n = 0;
    for (int k = 0; k < casts; k++) {
        m->effect_count = 0;
        spell_cast(c, sp, &g_game);
        n += !rules_has_condition_t(m->effects, m->effect_count, cond);
    }
    heard(c);
    return n;
}

static const SpellTemplate *spell_named(const char *name) {
    for (int i = 0; i < spell_database_size; i++) {
        if (strcmp(spell_database[i].name, name) == 0) {
            return &spell_database[i];
        }
    }
    return NULL;
}

int test_monster_magic(void) {
    hx_reset_world();
    Client *c = hero(0, "enchanter", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    const SpellTemplate *sp = spell_named("Charm Person");
    const SpellTemplate *blind = spell_named("Blindness/Deafness");
    CHECK(sp != NULL && blind != NULL, "no Charm Person, no Blindness/Deafness");
    NPC *m = monster_where(10, charm_proof);
    CHECK(m != NULL, "no monster immune to charm");
    spell_cast(c, sp, &g_game);
    heard(c);
    CHECK(said("cannot be charmed") &&
              !rules_has_condition_t(m->effects, m->effect_count, COND_CHARMED),
          "charmed a monster immune to it");
    m->active = false;

    /* The charm (its own saving throw) and a spell of the rules (the save
     * of Blindness/Deafness): more resisted with the advantage. */
    m = monster_where(11, plain);
    int plain_charm = resisted(c, m, sp, COND_CHARMED, 300);
    int plain_blind = resisted(c, m, blind, COND_BLINDED, 300);
    m->active = false;
    m = monster_where(12, magic_resistant);
    CHECK(m != NULL, "no magic-resistant monster");
    int mr_charm = resisted(c, m, sp, COND_CHARMED, 300);
    int mr_blind = resisted(c, m, blind, COND_BLINDED, 300);
    CHECK(mr_charm > plain_charm + 15 && mr_blind > plain_blind + 15,
          "magic resistance: charm %d/%d, blindness %d/%d resisted", mr_charm, plain_charm,
          mr_blind, plain_blind);
    return 0;
}

/* Hail of Thorns: the target and the creatures around it save or take the
 * thorns (it was one attack on one target); Staggering Smite's hit
 * frightens the target that fails its Wisdom save. */
int test_spell_weapon_riders(void) {
    hx_reset_world();
    Client *c = hero(0, "ranger", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    const SpellTemplate *hail = spell_named("Hail of Thorns");
    const SpellTemplate *smite = spell_named("Staggering Smite");
    CHECK(hail && smite, "no Hail of Thorns or Staggering Smite");
    NPC *a = monster(10, NULL, FLOOR, CX + 4, CY);
    NPC *b = monster(11, NULL, FLOOR, CX + 5, CY);
    a->hp = a->max_hp = b->hp = b->max_hp = 500;
    rules_force_d20(2); /* the saves fail */
    spell_cast(c, hail, &g_game);
    CHECK(a->hp < 500 && b->hp < 500, "the thorns: %d and %d HP", a->hp, b->hp);
    a->active = b->active = false;
    NPC *m = monster(12, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 500;
    m->ac = 1;
    rules_force_d20(2); /* AC 1: the attack hits; the save fails */
    spell_cast(c, smite, &g_game);
    rules_force_d20(0);
    heard(c);
    CHECK(m->hp < 500 && rules_has_condition_t(m->effects, m->effect_count, COND_FRIGHTENED),
          "the smite: HP %d, frightened %d", m->hp,
          rules_has_condition_t(m->effects, m->effect_count, COND_FRIGHTENED));
    return 0;
}
