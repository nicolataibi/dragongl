/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Text commands in the in-process fixture: shops, equipment, mining, the
 * DM commands and what each of them does with a number typed by a player.
 */

#ifndef DRAGONGL_HARNESS_COMMANDS_H
#define DRAGONGL_HARNESS_COMMANDS_H

int test_shop_no_arbitrage(void);
int test_shop_needs_a_merchant(void);
int test_wear_cursed_stays(void);
int test_merchant_restocks(void);
int test_boss_floor_is_playable(void);
int test_dm_spawn_and_place(void);
int test_numeric_arguments(void);
int test_tunnel_keeps_the_border(void);

#endif
