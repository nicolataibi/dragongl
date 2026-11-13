/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * In-process server fixture, shared by the integration harness
 * (server_harness.c) and the command fuzzer (tests/fuzz).
 *
 * Every server module but main_server.c is linked. fx_init() loads the
 * game data and creates the Game under test (g_game) with an empty world
 * and NPC table; players are backed by socketpair()s: the
 * server writes into one end, the fixture reads the other, as a TCP peer
 * would.
 */

#ifndef DRAGONGL_TEST_FIXTURE_H
#define DRAGONGL_TEST_FIXTURE_H

#include "game.h"
#include "server_entities.h"

#include <stdbool.h>
#include <stddef.h>

/*The game under test (one per process).*/
extern Game g_game;
extern Client g_test_clients[MAX_CLIENTS];
extern int g_peer_fd[MAX_CLIENTS]; /* fixture side of each socketpair, -1 if none */

/* libsodium, the state directory (created and locked), the game data from
 * data_dir, an empty world and NPC table. 0 on success, 1 on failure
 * (reported on stderr). */
int fx_init(const char *state_dir, const char *data_dir);

/* An empty item slot (template_idx -1). */
void fx_reset_item(ItemInstance *it);

/* A fresh, authenticated level-1 character on floor 0, with an empty
 * inventory and a live socket. NULL if the socketpair or the password
 * hash failed. */
Client *fx_player(int slot, const char *name);

/* Close the slot's socketpair and release its connection buffers. */
void fx_release(int slot);

/* Give the slot a new socketpair; the Client fields are kept. */
bool fx_reconnect(int slot);

/* Discard whatever the server wrote to this slot. */
void fx_drain(int slot);

/* Everything the server wrote to this slot since the last call, NUL
 * terminated; returns the byte count. */
size_t fx_output(int slot, char *buf, size_t cap);

#endif
