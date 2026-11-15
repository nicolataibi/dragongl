/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_utility.h - The spells of exploration, protection and support:
 * the handlers of the "special" spells that are not attacks (finding traps
 * and creatures, light, food, rest, curing, mending, digging, allies).
 * spell_router.c dispatches to them by the handler of the record.
 */

#ifndef SPELL_UTILITY_H
#define SPELL_UTILITY_H

#include <stdbool.h>

#include "spell_router.h"

bool spell_cleanse(SpellContext *ctx);
bool spell_find_traps(SpellContext *ctx);
bool spell_identify(SpellContext *ctx);
bool spell_light(SpellContext *ctx);
bool spell_mend(SpellContext *ctx);
bool spell_nourish(SpellContext *ctx);
bool spell_rally(SpellContext *ctx);
bool spell_remove_curse(SpellContext *ctx);
bool spell_sense(SpellContext *ctx);
bool spell_shape_earth(SpellContext *ctx);
bool spell_shelter(SpellContext *ctx);
bool spell_summon(SpellContext *ctx);

#endif /* SPELL_UTILITY_H */
