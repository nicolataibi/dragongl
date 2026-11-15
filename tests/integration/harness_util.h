/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Helpers shared by the harness modules written after the first ones
 * (combat, world, social, items): an open floor, characters and monsters
 * placed on it, items by name, a command run with what it answered.
 */

#ifndef DRAGONGL_HARNESS_UTIL_H
#define DRAGONGL_HARNESS_UTIL_H

#include "fixture.h"
#include "rules.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            rules_force_d20(0);                                                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

enum { HX_FLOOR = 1, HX_CX = 60, HX_CY = 60 };

/* What the last run() or heard() collected (binary frames included). */
extern char hx_out[1 << 16];
extern size_t hx_out_len;

/* True if the collected output contains needle (byte-wise). */
bool said(const char *needle);

/* Runs a text command of c; its answers end up in hx_out. */
void run(Client *c, const char *line);

/* Collects what the server wrote to c since the last drain. */
void heard(const Client *c);

/* Discards what the server wrote to c. */
void hush(const Client *c);

/* No NPCs, no tombstones, no traps; floors 0 to 2 and 10 open ground
 * with a rock border; the per-floor caches rebuilt. */
void hx_reset_world(void);

/* A level-5 character (all abilities 14, 60 HP, 1000 gold, nothing
 * carried) in slot `slot`, standing on (x, y) of floor. Created on the
 * first call for a slot, reset on the next ones. */
Client *hero(int slot, const char *name, int floor, int x, int y);

/* Moves c to (x, y) of floor, keeping the entity grid consistent. */
void place(Client *c, int floor, int x, int y);

/* A monster of the bestiary (the first whose name contains `name`, or
 * the first entry with NULL) in NPC slot `slot`, active, 30 HP, AC 10,
 * +2 to hit, 1d6 damage, waiting for a target. */
NPC *monster(int slot, const char *name, int floor, int x, int y);

/* Index of the first item whose name contains `part` (case ignored),
 * or -1; of the first item of a category, or -1. */
int item_named(const char *part);
int item_of(ItemCategory category);

/* Puts an identified copy of an item in c's backpack; returns it. */
ItemInstance *give(Client *c, int template_idx, int count);

/* True if a file whose name starts with prefix exists in the
 * subdirectory `dir` of the fixture's state directory (cwd-relative
 * path, e.g. "state-combat/saves"). */
bool file_with_prefix(const char *dir, const char *prefix);

/* Removes the files whose name starts with prefix from dir (what a
 * previous run of the same test left in its state directory). */
void remove_with_prefix(const char *dir, const char *prefix);

#endif
