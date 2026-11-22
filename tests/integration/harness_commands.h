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
int test_ammunition_bundles(void);
int test_shop_reactive_prices(void);
int test_shop_needs_a_merchant(void);
int test_wear_cursed_stays(void);
int test_merchant_restocks(void);
int test_boss_floor_is_playable(void);
int test_dm_spawn_and_place(void);
int test_numeric_arguments(void);
int test_tunnel_keeps_the_border(void);
int test_floors_without_characters_sleep(void);
int test_swarm_splits_into_free_slots(void);
int test_full_table_spares_the_boss(void);
int test_swarm_clones_stop_at_the_floor_cap(void);
int test_swarm_spares_the_dead_in_a_full_table(void);
int test_autosave_in_steps(void);
int test_metrics_and_telemetry(void);

#endif
