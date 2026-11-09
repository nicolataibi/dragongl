/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_rules.h - Resolution of a spell effect against one target, with
 * no world and no network: the server (spell_cast.c), the unit tests and
 * the balance check (tests/spell_balance.c) all run this same code.
 */

#ifndef SPELL_RULES_H
#define SPELL_RULES_H

#include <stdbool.h>

#include "classes.h"
#include "rules.h"
#include "spells.h"

typedef struct {
    int level;               /*character level, 1 or more*/
    int mods[ABILITY_COUNT]; /*ability modifiers (rules_get_modifier of the scores)*/
    Ability spell_ability;   /*class_spell_ability() of the caster's class*/
} SpellCaster;

typedef struct {
    int ac;
    int save_bonus;            /*added to each of its saving throws*/
    bool save_disadvantage;    /*e.g. cursed*/
    bool attack_advantage;     /*attacks against it have advantage*/
    bool attack_disadvantage;  /*e.g. the caster is blinded*/
    DamageModifier damage_mod; /*against the spell's damage type*/
    bool condition_immune;     /*against the spell's condition*/
} SpellTargetInfo;

typedef struct {
    bool landed;    /*attack hit, save failed, or no roll*/
    bool crit;      /*natural 20 on the attack: the dice double*/
    bool saved;     /*the saving throw (of the effect or of the rider) succeeded*/
    int roll;       /*the d20 of the attack or save; 0 when nothing was rolled*/
    int damage;     /*damage dealt (after save, crit and damage modifier)*/
    bool condition; /*the condition lands*/
} SpellOutcome;

/*The ability classes cast with (and martial classes fight with).*/
Ability class_spell_ability(ClassType cls);
/*Proficiency bonus by level: +2 at 1-4, +3 at 5-8, ... +6 at 17+.*/
int spell_proficiency(int level);
/*Modifier of a for this caster; ABILITY_SPELL is the casting ability.*/
int spell_ability_mod(const SpellCaster *c, Ability a);
/*The effect's fixed DC, else 8 + proficiency + casting ability modifier.*/
int spell_save_dc(const SpellEffect *e, const SpellCaster *c);
/*Proficiency + casting ability modifier.*/
int spell_attack_bonus(const SpellCaster *c);
/*Number of dice at this caster level (cantrips: x2 at 5, x3 at 11, x4 at 17).*/
int spell_dice_count(const SpellEffect *e, int level);
/*A roll of the effect's dice + bonus + dice_add modifier (never below 0);
 * crit doubles the dice.*/
int spell_roll_amount(const SpellEffect *e, const SpellCaster *c, bool crit);
/*Average of spell_roll_amount (no crit), for reports.*/
double spell_average_amount(const SpellEffect *e, const SpellCaster *c);

/*Resolves a damage or condition effect against one target: the roll, the
 * damage, and whether the condition lands. For any other kind only
 * `landed` is set (true).*/
SpellOutcome spell_resolve(const SpellEffect *e, const SpellCaster *c, const SpellTargetInfo *t);

#endif
