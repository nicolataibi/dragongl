/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Spell tests of the in-process harness (harness_spells.c). Each returns
 * 0 on success, 1 on failure.
 */

#ifndef DRAGONGL_HARNESS_SPELLS_H
#define DRAGONGL_HARNESS_SPELLS_H

int test_spell_attack(void);
int test_spell_save_area(void);
int test_spell_cone_and_line(void);
int test_spell_conditions(void);
int test_spell_self(void);
int test_spell_cloud(void);
int test_spell_special_and_terrain(void);
int test_spell_refusals(void);
int test_spell_every_effect(void);

#endif
