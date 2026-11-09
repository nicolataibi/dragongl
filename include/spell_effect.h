/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_effect.h - The "effect" object of data/spells.json: the names of
 * its enumerations and its parser. data/schema/spells.schema.json is the
 * specification; the parser rejects what the engine could not resolve.
 */

#ifndef SPELL_EFFECT_H
#define SPELL_EFFECT_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "spells.h"

/*Name <-> value of each enumeration, as the data spells them. The
 * *_from_name functions are case-sensitive (the data uses lower case) and
 * return the enumeration's COUNT value for an unknown name.*/
const char *spell_kind_name(SpellKind kind);
SpellKind spell_kind_from_name(const char *name); /*SPELL_KIND_COUNT if unknown*/
const char *spell_target_name(SpellTargetType target);
SpellTargetType spell_target_from_name(const char *name); /*SPELL_TARGET_COUNT if unknown*/
const char *spell_roll_name(SpellRoll roll);
SpellRoll spell_roll_from_name(const char *name); /*SPELL_ROLL_COUNT if unknown*/
const char *spell_handler_name(SpellHandler handler);
SpellHandler spell_handler_from_name(const char *name); /*SPELL_HANDLER_COUNT if unknown*/

/*True for the targets that cover an area (circle, cone, line, cloud).*/
bool spell_target_is_area(SpellTargetType target);

/*Fills *e from the effect object of one spell. On error returns false and
 * writes the reason ("effect.dice: missing") into err.*/
bool spell_effect_parse(SpellEffect *e, const cJSON *obj, char *err, size_t err_len);

#endif
