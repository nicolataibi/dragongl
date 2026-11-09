/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_cast.h - A spell cast in the world: the target, the area, the
 * effect on each creature (spell_rules.c resolves it), the messages and
 * the rewards.
 */

#ifndef SPELL_CAST_H
#define SPELL_CAST_H

#include <stdbool.h>

#include "server_entities.h"
#include "spell_rules.h"
#include "spells.h"

/*The caster's numbers: level, ability modifiers with the equipment, the
 * class casting ability.*/
void spell_caster_of(Client *c, SpellCaster *out);

/*Saving throw bonus of an NPC: +2, +1 every 10 floors (at most +9), +2
 * more for a boss.*/
int npc_save_bonus(const NPC *n);

/*A creature a player's spell can hit: alive, hostile (not a merchant,
 * treasure, gold pile, summoned or dominated ally).*/
bool spell_can_affect_npc(const NPC *n);

/*The effect of sp on one NPC: the roll, the damage, the condition (for
 * condition_rounds rounds, or the effect's rounds when 0), the line to
 * the caster and the kill reward. caster may be NULL (a cloud whose caster
 * left). True if the effect landed.*/
bool spell_affect_npc(const SpellTemplate *sp, const SpellCaster *sc, NPC *n, Client *caster,
                      int condition_rounds);

/*Casts sp: picks the target, resolves the effect, leaves the terrain.
 * False (after telling the caster why) when there is nothing to cast at:
 * the caller then keeps the spell slot.*/
bool spell_cast(Client *c, const SpellTemplate *sp, NPC *npcs);

#endif
