/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_rules.c - Resolution of a spell effect against one target.
 */

#include "spell_rules.h"

Ability class_spell_ability(ClassType cls) {
    switch (cls) {
    case CLASS_WIZARD:
        return ABILITY_INT;
    case CLASS_CLERIC:
    case CLASS_DRUID:
    case CLASS_RANGER:
    case CLASS_MONK:
        return ABILITY_WIS;
    case CLASS_BARD:
    case CLASS_SORCERER:
    case CLASS_WARLOCK:
    case CLASS_PALADIN:
        return ABILITY_CHA;
    case CLASS_ROGUE:
        return ABILITY_DEX;
    case CLASS_BARBARIAN:
    case CLASS_FIGHTER:
    default:
        return ABILITY_STR;
    }
}

int spell_proficiency(int level) {
    if (level < 1) {
        level = 1;
    }
    int p = 2 + (level - 1) / 4;
    return p > 6 ? 6 : p;
}

int spell_ability_mod(const SpellCaster *c, Ability a) {
    if (a == ABILITY_SPELL) {
        a = c->spell_ability;
    }
    if ((int)a < 0 || a >= ABILITY_COUNT) {
        return 0;
    }
    return c->mods[a];
}

int spell_save_dc(const SpellEffect *e, const SpellCaster *c) {
    if (e->dc > 0) {
        return e->dc;
    }
    return 8 + spell_proficiency(c->level) + spell_ability_mod(c, ABILITY_SPELL);
}

int spell_attack_bonus(const SpellCaster *c) {
    return spell_proficiency(c->level) + spell_ability_mod(c, ABILITY_SPELL);
}

int spell_dice_count(const SpellEffect *e, int level) {
    if (!e->cantrip_scaling) {
        return e->dice_count;
    }
    int tiers = 1 + (level >= 5) + (level >= 11) + (level >= 17);
    return e->dice_count * tiers;
}

static int amount_extra(const SpellEffect *e, const SpellCaster *c) {
    return e->dice_bonus + (e->dice_add != ABILITY_NONE ? spell_ability_mod(c, e->dice_add) : 0);
}

int spell_roll_amount(const SpellEffect *e, const SpellCaster *c, bool crit) {
    int count = spell_dice_count(e, c->level) * (crit ? 2 : 1);
    int total = (count > 0 && e->dice_sides > 0) ? rules_roll_dice(count, e->dice_sides) : 0;
    total += amount_extra(e, c);
    return total < 0 ? 0 : total;
}

double spell_average_amount(const SpellEffect *e, const SpellCaster *c) {
    double avg = spell_dice_count(e, c->level) * (e->dice_sides + 1) / 2.0;
    avg += amount_extra(e, c);
    return avg < 0.0 ? 0.0 : avg;
}

/*The rider of an attack or of an automatic effect: the condition lands
 * unless the target is immune or makes the rider's saving throw.*/
static bool rider_lands(const SpellEffect *e, const SpellCaster *c, const SpellTargetInfo *t,
                        SpellOutcome *o) {
    if (e->condition >= MAX_CONDITIONS || t->condition_immune) {
        return false;
    }
    if (e->roll == SPELL_ROLL_SAVE || e->save == ABILITY_NONE) {
        return true; /*the effect's own roll already decided*/
    }
    int roll = 0;
    o->saved =
        rules_roll_save(t->save_bonus, spell_save_dc(e, c), false, t->save_disadvantage, &roll);
    if (o->roll == 0) {
        o->roll = roll;
    }
    return !o->saved;
}

SpellOutcome spell_resolve(const SpellEffect *e, const SpellCaster *c, const SpellTargetInfo *t) {
    SpellOutcome o = {0};
    if (e->kind != SPELL_KIND_DAMAGE && e->kind != SPELL_KIND_CONDITION) {
        o.landed = true;
        return o;
    }
    switch (e->roll) {
    case SPELL_ROLL_ATTACK:
        o.landed = rules_roll_attack_detailed(spell_attack_bonus(c), t->ac, t->attack_advantage,
                                              t->attack_disadvantage, &o.crit, &o.roll);
        break;
    case SPELL_ROLL_SAVE:
        o.saved = rules_roll_save(t->save_bonus, spell_save_dc(e, c), false, t->save_disadvantage,
                                  &o.roll);
        o.landed = !o.saved;
        break;
    case SPELL_ROLL_AUTO:
    default:
        o.landed = true;
        break;
    }
    if (e->kind == SPELL_KIND_DAMAGE) {
        int raw = 0;
        if (o.landed) {
            raw = spell_roll_amount(e, c, o.crit);
        } else if (o.saved && e->save_halves) {
            raw = spell_roll_amount(e, c, false) / 2;
        }
        o.damage = rules_calculate_damage(raw, t->damage_mod);
    }
    o.condition = o.landed && rider_lands(e, c, t, &o);
    return o;
}
