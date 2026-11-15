/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Characters against monsters and characters with items: the rules of
 * LORE.md (conditions, wear, quality, artifacts) and of the commands.
 */

#ifndef DRAGONGL_HARNESS_INTERACT_H
#define DRAGONGL_HARNESS_INTERACT_H

int test_interact_conditions(void);
int test_interact_charm(void);
int test_interact_reach(void);
int test_interact_ranged(void);
int test_interact_ability_and_quality(void);
int test_interact_worn_bonuses(void);
int test_interact_artifacts(void);
int test_interact_loot_full_backpack(void);
int test_interact_ghosts(void);
int test_interact_light(void);

#endif
