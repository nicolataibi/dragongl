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
#include "persist.h"
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
int load_player_data(Client *c, const char *password);

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

static int fixture_init(const char *test_name) {
    if (!auth_init()) {
        fprintf(stderr, "libsodium initialization failed\n");
        return 1;
    }
    char err[256], dir[160];
    /* One state directory per test: each process locks its own. */
    snprintf(dir, sizeof(dir), "state-%s", test_name);
    if (!persist_init_state_dir(dir, err, sizeof(err))) {
        fprintf(stderr, "state directory: %s\n", err);
        return 1;
    }
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
    char pwd[40];
    snprintf(pwd, sizeof(pwd), "pw-%s", name);
    if (!auth_hash_password(pwd, c->password_hash))
        return NULL;
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
    char pwd[40];
    snprintf(pwd, sizeof(pwd), "pw-%s", c->username);
    memset(&loaded, 0, sizeof(loaded));
    snprintf(loaded.username, sizeof(loaded.username), "%s", c->username);
    CHECK(load_player_data(&loaded, "wrong-password") == -1, "wrong password accepted");
    int status = load_player_data(&loaded, pwd);
    CHECK(status == 1, "load_player_data returned %d", status);
    CHECK(loaded.backpack_count == count,
          "backpack_count after reload: expected %d, got %d", count,
          loaded.backpack_count);
    for (int i = 0; i < count; i++)
        CHECK(loaded.backpack[i].template_idx == c->backpack[i].template_idx,
              "item %d changed on reload", i);
    CHECK(strncmp(loaded.password_hash, "$argon2id$", 10) == 0,
          "password not stored as an Argon2id string");
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

/* --- File formats ------------------------------------------------------ */

static void flip_byte(const char *path, long offset) {
    FILE *f = fopen(path, "r+b");
    if (!f)
        return;
    fseek(f, offset, SEEK_SET);
    int ch = fgetc(f);
    fseek(f, offset, SEEK_SET);
    fputc(ch ^ 0xFF, f);
    fclose(f);
}

/* world.dat: tiles, traps and crystal respawns round-trip; a flipped byte
 * is detected. */
static int test_world_roundtrip(void) {
    static World saved, loaded;
    memset(&saved, 0, sizeof(saved));
    for (int f = 0; f < MAX_FLOORS; f++) {
        saved.floors[f].id = f;
        for (int y = 0; y < MAP_HEIGHT; y++)
            for (int x = 0; x < MAP_WIDTH; x++)
                saved.floors[f].map.data[0][y][x] = (VoxelType)((x * 7 + y * 13 + f) % 22);
    }
    Trap *t = &saved.floors[5].traps[0];
    saved.floors[5].trap_count = 1;
    t->type = TRAP_COUNT - 1;
    t->x = 10; t->y = 20; t->floor_id = 5; t->detection_dc = 12; t->save_dc = 14;
    t->damage_dice = 2; t->damage_sides = 6; t->damage_type = DMG_FIRE;
    t->respawn_timer = 33; t->active = true; t->detected = false; t->wall_dir = 3;
    saved.floors[7].crystal_respawn_count = 1;
    saved.floors[7].crystal_respawns[0] = (CrystalRespawn){.x = 1, .y = 2, .type = 17, .respawn_timer = 99};

    char path[512];
    CHECK(state_path(path, sizeof(path), "world.dat"), "state path");
    CHECK(persist_write_world(path, &saved), "write failed");
    memset(&loaded, 0xAB, sizeof(loaded));
    char why[96];
    CHECK(persist_read_world(path, &loaded, why, sizeof(why)) == BINFILE_OK, "read: %s", why);
    for (int f = 0; f < MAX_FLOORS; f++) {
        CHECK(loaded.floors[f].id == f, "floor id");
        CHECK(memcmp(loaded.floors[f].map.data, saved.floors[f].map.data,
                     sizeof(saved.floors[f].map.data)) == 0, "tiles of floor %d", f);
        CHECK(loaded.floors[f].entity_grid[0][0] == 0, "entity grid not cleared");
    }
    const Trap *lt = &loaded.floors[5].traps[0];
    CHECK(loaded.floors[5].trap_count == 1 && lt->type == t->type && lt->x == 10 && lt->y == 20 &&
          lt->save_dc == 14 && lt->damage_type == DMG_FIRE && lt->active && !lt->detected &&
          lt->wall_dir == 3, "trap fields");
    CHECK(loaded.floors[7].crystal_respawn_count == 1 &&
          loaded.floors[7].crystal_respawns[0].type == 17 &&
          loaded.floors[7].crystal_respawns[0].respawn_timer == 99, "crystal respawn");

    flip_byte(path, BINFILE_HEADER_SIZE + 1000);
    CHECK(persist_read_world(path, &loaded, why, sizeof(why)) == BINFILE_INVALID,
          "corruption not detected");
    return 0;
}

/* npcs.dat: records keep their slot and their fields; transient state
 * (effects, template pointer) is not stored. */
static int test_npcs_roundtrip(void) {
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    NPC *a = &g_npcs[3];
    a->entity_id = 42; a->x = 11; a->y = 12; a->floor_id = 4; a->hp = 30; a->max_hp = 40;
    a->active = true; a->archetype = ARCH_MERCHANT; a->template_idx = 2; a->gold_drop = 77;
    a->ai_ctx.behavior = AI_BEHAVIOR_MAGE; a->merchant.item_count = 2;
    a->merchant.item_templates[1] = 9; a->merchant.item_stock[1] = 5;
    snprintf(a->merchant.shop_name, sizeof(a->merchant.shop_name), "The Iron Anvil");
    a->merchant.spec = BOOKS_MARTIAL; a->respawn_timer = -1; a->energy = 3;
    a->ghost_loot[29].template_idx = 1; a->ghost_loot[29].stack_count = 2;
    a->effect_count = 1; /* transient: must not survive */
    NPC *b = &g_npcs[MAX_NPCS - 1];
    b->entity_id = 43; b->floor_id = 100; b->x = 299; b->y = 299;
    b->template = (const MonsterTemplate *)1; /* inactive but kept (respawning) */
    b->respawn_timer = 50;

    char path[512];
    CHECK(state_path(path, sizeof(path), "npcs.dat"), "state path");
    CHECK(persist_write_npcs(path, g_npcs, MAX_NPCS, 1234, 5678), "write failed");
    NPC *loaded = calloc(MAX_NPCS, sizeof(NPC));
    CHECK(loaded != NULL, "out of memory");
    int next = 0, turns = 0, count = 0;
    char why[96];
    BinFileStatus st = persist_read_npcs(path, loaded, MAX_NPCS, &next, &turns, &count, why, sizeof(why));
    int rc = 0;
    if (st != BINFILE_OK) { fprintf(stderr, "FAIL read: %s\n", why); rc = 1; }
    else if (count != 2 || next != 1234 || turns != 5678) { fprintf(stderr, "FAIL header\n"); rc = 1; }
    else {
        const NPC *la = &loaded[3], *lb = &loaded[MAX_NPCS - 1];
        if (la->entity_id != 42 || la->x != 11 || la->hp != 30 || !la->active ||
            la->archetype != ARCH_MERCHANT || la->ai_ctx.behavior != AI_BEHAVIOR_MAGE ||
            la->merchant.item_count != 2 || la->merchant.item_stock[1] != 5 ||
            strcmp(la->merchant.shop_name, "The Iron Anvil") != 0 ||
            la->merchant.spec != BOOKS_MARTIAL || la->ghost_loot[29].stack_count != 2 ||
            la->effect_count != 0 || la->template != NULL) {
            fprintf(stderr, "FAIL npc 3 fields\n"); rc = 1;
        }
        if (lb->entity_id != 43 || lb->active || lb->respawn_timer != 50 || lb->floor_id != 100) {
            fprintf(stderr, "FAIL npc last fields\n"); rc = 1;
        }
    }
    free(loaded);
    b->template = NULL;
    return rc;
}

static int test_tombstone_and_flags_roundtrip(void) {
    Tombstone t;
    memset(&t, 0, sizeof(t));
    t.active = true; t.x = 5; t.y = 6; t.floor_id = 7; t.gold = 1ULL << 40;
    t.death_time = 1700000000; t.backpack_count = 1;
    snprintf(t.owner, sizeof(t.owner), "fallen");
    t.backpack[0].template_idx = 3; t.backpack[0].is_artifact = true;
    snprintf(t.backpack[0].artifact_name, sizeof(t.backpack[0].artifact_name), "Void Blade");
    t.slot_rings[9].template_idx = 4;
    char path[512];
    CHECK(state_path(path, sizeof(path), "saves/tombstone_test.dat"), "state path");
    CHECK(persist_write_tombstone(path, &t), "tombstone write");
    Tombstone l;
    char why[96];
    CHECK(persist_read_tombstone(path, &l, why, sizeof(why)) == BINFILE_OK, "tombstone read: %s", why);
    CHECK(l.gold == t.gold && l.death_time == t.death_time && strcmp(l.owner, "fallen") == 0 &&
          l.backpack[0].is_artifact && strcmp(l.backpack[0].artifact_name, "Void Blade") == 0 &&
          l.slot_rings[9].template_idx == 4, "tombstone fields");
    remove(path);

    bool flags[5] = {true, false, true, true, false}, back[5] = {false};
    CHECK(state_path(path, sizeof(path), "flags_test.dat"), "state path");
    CHECK(persist_write_flags(path, flags, 5), "flags write");
    CHECK(persist_read_flags(path, back, 5, why, sizeof(why)) == BINFILE_OK, "flags read");
    CHECK(memcmp(flags, back, sizeof(flags)) == 0, "flags differ");
    CHECK(persist_read_flags(path, back, 4, why, sizeof(why)) == BINFILE_INVALID,
          "a different artifact count must be refused");
    remove(path);
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
    {"world_roundtrip", test_world_roundtrip},
    {"npcs_roundtrip", test_npcs_roundtrip},
    {"tombstone_and_flags_roundtrip", test_tombstone_and_flags_roundtrip},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <test>\n", argv[0]);
        for (size_t i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++)
            fprintf(stderr, "  %s\n", k_tests[i].name);
        return 2;
    }
    if (fixture_init(argv[1]) != 0)
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
