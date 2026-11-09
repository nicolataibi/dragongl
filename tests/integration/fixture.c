/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * In-process server fixture (see fixture.h).
 */

#include "fixture.h"

#include "auth.h"
#include "data_loader.h"
#include "persist.h"
#include "server_conn.h"
#include "server_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

Client g_test_clients[MAX_CLIENTS];
int g_peer_fd[MAX_CLIENTS];

int fx_init(const char *state_dir, const char *data_dir) {
    if (!auth_init()) {
        fprintf(stderr, "libsodium initialization failed\n");
        return 1;
    }
    char err[256];
    if (!persist_init_state_dir(state_dir, err, sizeof(err))) {
        fprintf(stderr, "state directory: %s\n", err);
        return 1;
    }
    if (!init_data_loaders(data_dir)) {
        fprintf(stderr, "cannot load game data from %s\n", data_dir);
        return 1;
    }
    master_world = calloc(1, sizeof(World));
    g_npcs = calloc(MAX_NPCS, sizeof(NPC));
    if (!master_world || !g_npcs) {
        return 1;
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        g_npcs[i].respawn_timer = -1;
    }
    memset(g_test_clients, 0, sizeof(g_test_clients));
    g_clients = g_test_clients;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_peer_fd[i] = -1;
    }
    return 0;
}

void fx_reset_item(ItemInstance *it) {
    memset(it, 0, sizeof(*it));
    it->template_idx = -1;
}

bool fx_reconnect(int slot) {
    Client *c = &g_test_clients[slot];
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return false;
    }
    (void)fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
    c->sock = sv[0];
    g_peer_fd[slot] = sv[1];
    conn_open(slot, sv[0], "127.0.0.1", 0);
    return true;
}

void fx_release(int slot) {
    Client *c = &g_test_clients[slot];
    if (conn_is_open(slot)) {
        conn_release(slot);
    }
    if (c->sock > 0) {
        close(c->sock);
        c->sock = -1;
    }
    if (g_peer_fd[slot] >= 0) {
        close(g_peer_fd[slot]);
        g_peer_fd[slot] = -1;
    }
}

Client *fx_player(int slot, const char *name) {
    Client *c = &g_test_clients[slot];
    memset(c, 0, sizeof(*c));
    if (!fx_reconnect(slot)) {
        return NULL;
    }
    c->active = true;
    c->authenticated = true;
    c->entity_id = 1000 + slot;
    snprintf(c->username, sizeof(c->username), "%s", name);
    char pwd[40];
    snprintf(pwd, sizeof(pwd), "pw-%s", name);
    if (!auth_hash_password(pwd, c->password_hash)) {
        return NULL;
    }
    c->level = 1;
    c->hp = c->max_hp = 20;
    c->str = c->dex = c->con = c->intel = c->wis = c->cha = 10;
    c->x = MAP_CENTER_X;
    c->y = MAP_CENTER_Y;
    c->floor_id = 0;
    c->pending_trade_merchant_id = -1;
    c->trade_offer_item_idx = -1;
    for (int i = 0; i < MAX_BACKPACK; i++) {
        fx_reset_item(&c->backpack[i]);
    }
    for (int i = 0; i < MAX_BELT; i++) {
        fx_reset_item(&c->belt[i]);
    }
    for (int i = 0; i < 10; i++) {
        fx_reset_item(&c->slot_rings[i]);
    }
    ItemInstance *slots[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                             &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                             &c->slot_arm_l,  &c->slot_feet};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        fx_reset_item(slots[i]);
    }
    return c;
}

void fx_drain(int slot) {
    char buf[4096];
    conn_flush(slot);
    while (g_peer_fd[slot] >= 0 && read(g_peer_fd[slot], buf, sizeof(buf)) > 0) {
    }
}

size_t fx_output(int slot, char *buf, size_t cap) {
    size_t n = 0;
    conn_flush(slot);
    ssize_t r;
    while (n + 1 < cap && g_peer_fd[slot] >= 0 &&
           (r = read(g_peer_fd[slot], buf + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
    }
    buf[n] = '\0';
    return n;
}
