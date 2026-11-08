/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * In-process server test harness.
 *
 * Links every server translation unit (built with -DDRAGONGL_NO_MAIN) and
 * drives the real functions directly. Each test is selected by name on
 * the command line and returns 0 on success, 1 on failure, so CTest can
 * register one test per scenario.
 *
 * Clients are backed by socketpair()s: the server writes its text
 * replies into one end and the harness drains the other, exactly as a
 * TCP peer would.
 */

#include "data_loader.h"
#include "server_conn.h"
#include "server_commands.h"
#include "server_entities.h"
#include "server_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void save_player_data(Client *c);
int load_player_data(Client *c);

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            return 1;                                                      \
        }                                                                  \
    } while (0)

/* --- Fixture -------------------------------------------------------- */

static Client g_test_clients[MAX_CLIENTS];
static int g_peer_fd[MAX_CLIENTS]; /* harness side of each socketpair */

static void reset_item(ItemInstance *it) {
    memset(it, 0, sizeof(*it));
    it->template_idx = -1;
}

static int fixture_init(void) {
    if (!init_data_loaders(DGL_TEST_DATA_DIR)) {
        fprintf(stderr, "cannot load game data from %s\n", DGL_TEST_DATA_DIR);
        return 1;
    }
    master_world = calloc(1, sizeof(World));
    g_npcs = calloc(MAX_NPCS, sizeof(NPC));
    if (!master_world || !g_npcs)
        return 1;
    for (int i = 0; i < MAX_NPCS; i++)
        g_npcs[i].respawn_timer = -1;
    memset(g_test_clients, 0, sizeof(g_test_clients));
    g_clients = g_test_clients;
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_peer_fd[i] = -1;
    return 0;
}

/* A fresh, authenticated level-1 character on floor 0, with an empty
 * inventory and a live socket. */
static Client *make_player(int slot, const char *name) {
    Client *c = &g_test_clients[slot];
    memset(c, 0, sizeof(*c));
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return NULL;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
    c->sock = sv[0];
    g_peer_fd[slot] = sv[1];
    conn_open(slot, sv[0], "127.0.0.1", 0);
    c->active = true;
    c->authenticated = true;
    c->entity_id = 1000 + slot;
    snprintf(c->username, sizeof(c->username), "%s", name);
    snprintf(c->password, sizeof(c->password), "pw-%s", name);
    c->level = 1;
    c->hp = c->max_hp = 20;
    c->str = c->dex = c->con = c->intel = c->wis = c->cha = 10;
    c->x = MAP_CENTER_X;
    c->y = MAP_CENTER_Y;
    c->floor_id = 0;
    c->pending_trade_merchant_id = -1;
    c->trade_offer_item_idx = -1;
    for (int i = 0; i < MAX_BACKPACK; i++)
        reset_item(&c->backpack[i]);
    for (int i = 0; i < MAX_BELT; i++)
        reset_item(&c->belt[i]);
    for (int i = 0; i < 10; i++)
        reset_item(&c->slot_rings[i]);
    ItemInstance *slots[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,
                             &c->slot_back,   &c->slot_hand_r, &c->slot_hand_l,
                             &c->slot_hands,  &c->slot_arm_r,  &c->slot_arm_l,
                             &c->slot_feet};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        reset_item(slots[i]);
    return c;
}

static void give_item(Client *c, int template_idx) {
    ItemInstance *it = &c->backpack[c->backpack_count++];
    reset_item(it);
    it->template_idx = template_idx;
    it->stack_count = 1;
    it->is_identified = true;
}

/* Discard whatever the server wrote to this client. */
static void drain(int slot) {
    char buf[4096];
    conn_flush(slot);
    while (g_peer_fd[slot] >= 0 && read(g_peer_fd[slot], buf, sizeof(buf)) > 0) {
    }
}

static void cmd(Client *c, const char *text) {
    handle_text_cmd(c, text, g_npcs);
    drain((int)(c - g_test_clients));
}

/* --- Persistence ------------------------------------------------------ */

static int backpack_roundtrip(int count) {
    Client *c = make_player(0, count == MAX_BACKPACK ? "fullpack" : "partpack");
    CHECK(c != NULL, "socketpair failed");
    for (int i = 0; i < count; i++)
        give_item(c, i % item_database_size);
    save_player_data(c);

    static Client loaded;
    memset(&loaded, 0, sizeof(loaded));
    snprintf(loaded.username, sizeof(loaded.username), "%s", c->username);
    snprintf(loaded.password, sizeof(loaded.password), "%s", c->password);
    int status = load_player_data(&loaded);
    CHECK(status == 1, "load_player_data returned %d", status);
    CHECK(loaded.backpack_count == count,
          "backpack_count after reload: expected %d, got %d", count,
          loaded.backpack_count);
    for (int i = 0; i < count; i++)
        CHECK(loaded.backpack[i].template_idx == c->backpack[i].template_idx,
              "item %d changed on reload", i);
    return 0;
}

/* P1-1: a full backpack (32/32) is a legal state and must survive a
 * save/load round trip. */
static int test_full_backpack_roundtrip(void) {
    return backpack_roundtrip(MAX_BACKPACK);
}

/* Control case: a partially filled backpack. */
static int test_partial_backpack_roundtrip(void) {
    return backpack_roundtrip(MAX_BACKPACK - 1);
}

/* --- Trade ------------------------------------------------------------ */

/* P1-3: alice offers her 3rd item; bob accepts; alice then drops every
 * item she owns and accepts. The trade must not hand bob an item alice
 * no longer has, and alice's inventory must stay consistent. */
static int test_trade_offer_revalidated_on_accept(void) {
    Client *alice = make_player(0, "alice");
    Client *bob = make_player(1, "bob");
    CHECK(alice && bob, "socketpair failed");
    give_item(alice, 0);
    give_item(alice, 1);
    give_item(alice, 2);

    cmd(alice, "trade bob");
    cmd(bob, "trade alice");
    cmd(alice, "offer 0 3");
    CHECK(alice->trade_offer_item_idx == 2, "offer not registered");
    cmd(bob, "accept");

    cmd(alice, "drop 1");
    cmd(alice, "drop 1");
    cmd(alice, "drop 1");
    CHECK(alice->backpack_count == 0, "drop did not empty the backpack (%d left)",
          alice->backpack_count);

    cmd(alice, "accept");

    CHECK(alice->backpack_count >= 0, "alice backpack_count went negative (%d)",
          alice->backpack_count);
    CHECK(bob->backpack_count == 0,
          "bob received %d item(s) that alice no longer owned (item duplication)",
          bob->backpack_count);
    return 0;
}

/* Control case: a plain exchange moves the item one way and the gold the
 * other, and nothing is created or destroyed. */
static int test_trade_moves_item_and_gold(void) {
    Client *alice = make_player(0, "alice");
    Client *bob = make_player(1, "bob");
    CHECK(alice && bob, "socketpair failed");
    give_item(alice, 0);
    give_item(alice, 1);
    int offered = alice->backpack[1].template_idx;
    bob->gold = 500;

    cmd(alice, "trade bob");
    cmd(bob, "trade alice");
    cmd(alice, "offer 0 2");
    cmd(bob, "offer 300");
    cmd(alice, "accept");
    cmd(bob, "accept");

    CHECK(alice->backpack_count == 1, "alice should keep 1 item, has %d", alice->backpack_count);
    CHECK(bob->backpack_count == 1, "bob should have 1 item, has %d", bob->backpack_count);
    CHECK(bob->backpack[0].template_idx == offered, "bob got the wrong item");
    CHECK(alice->gold == 300 && bob->gold == 200, "gold not exchanged (alice %llu, bob %llu)",
          (unsigned long long)alice->gold, (unsigned long long)bob->gold);
    CHECK(alice->trading_with[0] == '\0' && bob->trading_with[0] == '\0', "trade not closed");
    return 0;
}

/* A receiver with a full backpack: the trade is cancelled, nothing is
 * overwritten (the old code replaced backpack slot 31). */
static int test_trade_into_full_backpack_is_cancelled(void) {
    Client *alice = make_player(0, "alice");
    Client *bob = make_player(1, "bob");
    CHECK(alice && bob, "socketpair failed");
    give_item(alice, 2);
    for (int i = 0; i < MAX_BACKPACK; i++)
        give_item(bob, 3);
    int last = bob->backpack[MAX_BACKPACK - 1].template_idx;

    cmd(alice, "trade bob");
    cmd(bob, "trade alice");
    cmd(alice, "offer 0 1");
    cmd(bob, "accept");
    cmd(alice, "accept");

    CHECK(alice->backpack_count == 1, "alice lost her item");
    CHECK(bob->backpack_count == MAX_BACKPACK, "bob backpack_count %d", bob->backpack_count);
    CHECK(bob->backpack[MAX_BACKPACK - 1].template_idx == last, "bob's last item was overwritten");
    return 0;
}

/* --- Runner ------------------------------------------------------------ */

typedef struct {
    const char *name;
    int (*fn)(void);
} TestCase;

static const TestCase k_tests[] = {
    {"full_backpack_roundtrip", test_full_backpack_roundtrip},
    {"partial_backpack_roundtrip", test_partial_backpack_roundtrip},
    {"trade_offer_revalidated_on_accept", test_trade_offer_revalidated_on_accept},
    {"trade_moves_item_and_gold", test_trade_moves_item_and_gold},
    {"trade_into_full_backpack_is_cancelled", test_trade_into_full_backpack_is_cancelled},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <test>\n", argv[0]);
        for (size_t i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++)
            fprintf(stderr, "  %s\n", k_tests[i].name);
        return 2;
    }
    if (fixture_init() != 0)
        return 1;
    for (size_t i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++) {
        if (strcmp(argv[1], k_tests[i].name) == 0) {
            int rc = k_tests[i].fn();
            printf("%s: %s\n", k_tests[i].name, rc == 0 ? "PASS" : "FAIL");
            return rc;
        }
    }
    fprintf(stderr, "unknown test '%s'\n", argv[1]);
    return 2;
}
