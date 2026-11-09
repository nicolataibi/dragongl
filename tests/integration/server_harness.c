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
 * register one test per scenario. The world, the NPC table and the
 * socketpair-backed players come from fixture.h.
 */

#include "fixture.h"
#include "harness_spells.h"

#include "binio.h"
#include "classes.h"
#include "data_loader.h"
#include "items.h"
#include "spells.h"
#include "login_worker.h"
#include "persist.h"
#include "server_conn.h"
#include "server_commands.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_cmd.h"
#include "server_move.h"
#include "server_spawn.h"
#include "server_world.h"
#include "writer.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* --- Fixture: fixture.h ---------------------------------------------- */

static void give_item(Client *c, int template_idx) {
    ItemInstance *it = &c->backpack[c->backpack_count++];
    fx_reset_item(it);
    it->template_idx = template_idx;
    it->stack_count = 1;
    it->is_identified = true;
}

static void cmd(Client *c, const char *text) {
    handle_text_cmd(c, text, g_npcs);
    fx_drain((int)(c - g_test_clients));
}

/* --- Persistence ------------------------------------------------------ */

static int backpack_roundtrip(int count) {
    Client *c = fx_player(0, count == MAX_BACKPACK ? "fullpack" : "partpack");
    CHECK(c != NULL, "socketpair failed");
    for (int i = 0; i < count; i++) {
        give_item(c, i % item_database_size);
    }
    save_player_data(c);

    static Client loaded;
    char pwd[40];
    snprintf(pwd, sizeof(pwd), "pw-%s", c->username);
    memset(&loaded, 0, sizeof(loaded));
    snprintf(loaded.username, sizeof(loaded.username), "%s", c->username);
    CHECK(load_player_data(&loaded, "wrong-password") == -1, "wrong password accepted");
    int status = load_player_data(&loaded, pwd);
    CHECK(status == 1, "load_player_data returned %d", status);
    CHECK(loaded.backpack_count == count, "backpack_count after reload: expected %d, got %d", count,
          loaded.backpack_count);
    for (int i = 0; i < count; i++) {
        CHECK(loaded.backpack[i].template_idx == c->backpack[i].template_idx,
              "item %d changed on reload", i);
    }
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
    Client *alice = fx_player(0, "alice");
    Client *bob = fx_player(1, "bob");
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
    Client *alice = fx_player(0, "alice");
    Client *bob = fx_player(1, "bob");
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
    Client *alice = fx_player(0, "alice");
    Client *bob = fx_player(1, "bob");
    CHECK(alice && bob, "socketpair failed");
    give_item(alice, 2);
    for (int i = 0; i < MAX_BACKPACK; i++) {
        give_item(bob, 3);
    }
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
    if (!f) {
        return;
    }
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
        for (int y = 0; y < MAP_HEIGHT; y++) {
            for (int x = 0; x < MAP_WIDTH; x++) {
                saved.floors[f].map.data[0][y][x] = (VoxelType)((x * 7 + y * 13 + f) % 22);
            }
        }
    }
    Trap *t = &saved.floors[5].traps[0];
    saved.floors[5].trap_count = 1;
    t->type = TRAP_COUNT - 1;
    t->x = 10;
    t->y = 20;
    t->floor_id = 5;
    t->detection_dc = 12;
    t->save_dc = 14;
    t->damage_dice = 2;
    t->damage_sides = 6;
    t->damage_type = DMG_FIRE;
    t->respawn_timer = 33;
    t->active = true;
    t->detected = false;
    t->wall_dir = 3;
    saved.floors[7].crystal_respawn_count = 1;
    saved.floors[7].crystal_respawns[0] =
        (CrystalRespawn){.x = 1, .y = 2, .type = 17, .respawn_timer = 99};

    char path[512];
    CHECK(state_path(path, sizeof(path), "world.dat"), "state path");
    CHECK(persist_write_world(path, &saved), "write failed");
    memset(&loaded, 0xAB, sizeof(loaded));
    char why[96];
    CHECK(persist_read_world(path, &loaded, why, sizeof(why)) == BINFILE_OK, "read: %s", why);
    for (int f = 0; f < MAX_FLOORS; f++) {
        CHECK(loaded.floors[f].id == f, "floor id");
        CHECK(memcmp(loaded.floors[f].map.data, saved.floors[f].map.data,
                     sizeof(saved.floors[f].map.data)) == 0,
              "tiles of floor %d", f);
        CHECK(loaded.floors[f].entity_grid[0][0] == 0, "entity grid not cleared");
    }
    const Trap *lt = &loaded.floors[5].traps[0];
    CHECK(loaded.floors[5].trap_count == 1 && lt->type == t->type && lt->x == 10 && lt->y == 20 &&
              lt->save_dc == 14 && lt->damage_type == DMG_FIRE && lt->active && !lt->detected &&
              lt->wall_dir == 3,
          "trap fields");
    CHECK(loaded.floors[7].crystal_respawn_count == 1 &&
              loaded.floors[7].crystal_respawns[0].type == 17 &&
              loaded.floors[7].crystal_respawns[0].respawn_timer == 99,
          "crystal respawn");

    flip_byte(path, BINFILE_HEADER_SIZE + 1000);
    CHECK(persist_read_world(path, &loaded, why, sizeof(why)) == BINFILE_INVALID,
          "corruption not detected");
    return 0;
}

/* npcs.dat: records keep their slot and their fields; transient state
 * (effects, template pointer) is not stored. */
/* Overwrite the little-endian u32 at payload offset `at` of a container
 * file (binio.h) and fix its CRC, as a writer of other data would have. */
static bool rewrite_payload_u32(const char *path, size_t at, uint32_t v) {
    FILE *f = fopen(path, "r+b");
    if (!f) {
        return false;
    }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    bool ok = n >= BINFILE_HEADER_SIZE + at + 4;
    if (ok) {
        uint8_t *p = buf + BINFILE_HEADER_SIZE;
        for (int i = 0; i < 4; i++) {
            p[at + (size_t)i] = (uint8_t)(v >> (8 * i));
        }
        uint32_t crc = crc32_data(p, n - BINFILE_HEADER_SIZE);
        for (int i = 0; i < 4; i++) {
            buf[12 + i] = (uint8_t)(crc >> (8 * i));
        }
        ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(buf, 1, n, f) == n;
    }
    return fclose(f) == 0 && ok;
}

static int test_npcs_roundtrip(void) {
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    NPC *a = &g_npcs[3];
    a->entity_id = 42;
    a->x = 11;
    a->y = 12;
    a->floor_id = 4;
    a->hp = 30;
    a->max_hp = 40;
    a->active = true;
    a->archetype = ARCH_MERCHANT;
    a->template_idx = 2;
    a->gold_drop = 77;
    a->ai_ctx.behavior = AI_BEHAVIOR_MAGE;
    a->merchant.item_count = 2;
    a->merchant.item_templates[1] = 9;
    a->merchant.item_stock[1] = 5;
    snprintf(a->merchant.shop_name, sizeof(a->merchant.shop_name), "The Iron Anvil");
    a->merchant.spec = BOOKS_MARTIAL;
    a->respawn_timer = -1;
    a->energy = 3;
    a->ghost_loot[29].template_idx = 1;
    a->ghost_loot[29].stack_count = 2;
    a->effect_count = 1; /* transient: must not survive */
    NPC *b = &g_npcs[MAX_NPCS - 1];
    b->entity_id = 43;
    b->floor_id = 100;
    b->x = 299;
    b->y = 299;
    b->template = (const MonsterTemplate *)1; /* inactive but kept (respawning) */
    b->respawn_timer = 50;

    char path[512];
    CHECK(state_path(path, sizeof(path), "npcs.dat"), "state path");
    CHECK(persist_write_npcs(path, g_npcs, MAX_NPCS, 1234, 5678), "write failed");
    NPC *loaded = calloc(MAX_NPCS, sizeof(NPC));
    CHECK(loaded != NULL, "out of memory");
    int next = 0, turns = 0, count = 0;
    char why[96];
    BinFileStatus st =
        persist_read_npcs(path, loaded, MAX_NPCS, &next, &turns, &count, why, sizeof(why));
    int rc = 0;
    if (st != BINFILE_OK) {
        fprintf(stderr, "FAIL read: %s\n", why);
        rc = 1;
    } else if (count != 2 || next != 1234 || turns != 5678) {
        fprintf(stderr, "FAIL header\n");
        rc = 1;
    } else {
        const NPC *la = &loaded[3], *lb = &loaded[MAX_NPCS - 1];
        if (la->entity_id != 42 || la->x != 11 || la->hp != 30 || !la->active ||
            la->archetype != ARCH_MERCHANT || la->ai_ctx.behavior != AI_BEHAVIOR_MAGE ||
            la->merchant.item_count != 2 || la->merchant.item_stock[1] != 5 ||
            strcmp(la->merchant.shop_name, "The Iron Anvil") != 0 ||
            la->merchant.spec != BOOKS_MARTIAL || la->ghost_loot[29].stack_count != 2 ||
            la->effect_count != 0 || la->template != NULL) {
            fprintf(stderr, "FAIL npc 3 fields\n");
            rc = 1;
        }
        if (lb->entity_id != 43 || lb->active || lb->respawn_timer != 50 || lb->floor_id != 100) {
            fprintf(stderr, "FAIL npc last fields\n");
            rc = 1;
        }
    }
    /* Saved NPCs refer to monsters by index: a file written for another
     * bestiary (a monster added, removed or renamed) is refused. The
     * fingerprint follows the NPC capacity in the payload. */
    if (rc == 0 && !rewrite_payload_u32(path, 4, bestiary_fingerprint() ^ 1U)) {
        fprintf(stderr, "FAIL: cannot rewrite npcs.dat\n");
        rc = 1;
    } else if (rc == 0) {
        memset(loaded, 0, sizeof(NPC) * MAX_NPCS);
        st = persist_read_npcs(path, loaded, MAX_NPCS, &next, &turns, &count, why, sizeof(why));
        if (st != BINFILE_INVALID || strstr(why, "another bestiary") == NULL) {
            fprintf(stderr, "FAIL: npcs.dat of another bestiary accepted (%d)\n", (int)st);
            rc = 1;
        }
    }
    free(loaded);
    b->template = NULL;
    return rc;
}

static int test_tombstone_and_flags_roundtrip(void) {
    Tombstone t;
    memset(&t, 0, sizeof(t));
    t.active = true;
    t.x = 5;
    t.y = 6;
    t.floor_id = 7;
    t.gold = 1ULL << 40;
    t.death_time = 1700000000;
    t.backpack_count = 1;
    snprintf(t.owner, sizeof(t.owner), "fallen");
    t.backpack[0].template_idx = 3;
    t.backpack[0].is_artifact = true;
    snprintf(t.backpack[0].artifact_name, sizeof(t.backpack[0].artifact_name), "Void Blade");
    t.slot_rings[9].template_idx = 4;
    char path[512];
    CHECK(state_path(path, sizeof(path), "saves/tombstone_test.dat"), "state path");
    CHECK(persist_write_tombstone(path, &t), "tombstone write");
    Tombstone l;
    char why[96];
    CHECK(persist_read_tombstone(path, &l, why, sizeof(why)) == BINFILE_OK, "tombstone read: %s",
          why);
    CHECK(l.gold == t.gold && l.death_time == t.death_time && strcmp(l.owner, "fallen") == 0 &&
              l.backpack[0].is_artifact && strcmp(l.backpack[0].artifact_name, "Void Blade") == 0 &&
              l.slot_rings[9].template_idx == 4,
          "tombstone fields");
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

/* --- Writer thread (phase 4b) ---------------------------------------- */

static int read_flag0(const char *path, bool *value) {
    bool f[1] = {false};
    char why[96];
    if (persist_read_flags(path, f, 1, why, sizeof(why)) != BINFILE_OK) {
        return -1;
    }
    *value = f[0];
    return 0;
}

static int test_writer_queue(void) {
    char path[512];
    CHECK(state_path(path, sizeof(path), "writer_test.dat"), "state path");
    CHECK(writer_start(), "writer_start");

    /* Many snapshots of one file: queued jobs are replaced, the last one
     * wins, and writer_wait_for() returns once it is on disk. */
    for (int i = 0; i < 200; i++) {
        bool v[1] = {(i % 2) == 1};
        CHECK(persist_write_flags(path, v, 1), "queue %d", i);
    }
    writer_wait_for(path);
    bool got = false;
    CHECK(read_flag0(path, &got) == 0 && got, "the last snapshot (true) is not on disk");

    /* Write then unlink: removed. Unlink then write: present. */
    bool t[1] = {true};
    CHECK(persist_write_flags(path, t, 1), "write");
    CHECK(writer_submit_unlink(path), "unlink");
    writer_wait_for(path);
    CHECK(access(path, F_OK) != 0, "file still present after a queued unlink");
    CHECK(writer_submit_unlink(path), "unlink");
    CHECK(persist_write_flags(path, t, 1), "write");
    writer_wait_for(path);
    CHECK(access(path, F_OK) == 0, "file missing after unlink+write");

    /* A save followed at once by a load sees the new contents. */
    Client *c = fx_player(0, "quick");
    CHECK(c != NULL, "socketpair failed");
    for (int round = 1; round <= 5; round++) {
        c->gold = (uint64_t)round * 1000;
        save_player_data(c);
        static Client loaded;
        memset(&loaded, 0, sizeof(loaded));
        snprintf(loaded.username, sizeof(loaded.username), "quick");
        CHECK(load_player_data(&loaded, "pw-quick") == 1, "load");
        CHECK(loaded.gold == c->gold, "round %d: loaded gold %lu, saved %lu", round,
              (unsigned long)loaded.gold, (unsigned long)c->gold);
    }

    /* A failing write is counted and keeps the previous file. */
    unsigned failures = writer_failures();
    char bad[600];
    CHECK(state_path(bad, sizeof(bad), "no-such-dir/x.dat"), "state path");
    CHECK(persist_write_flags(bad, t, 1), "queued");
    writer_wait_for(bad);
    CHECK(writer_failures() == failures + 1, "failure not counted");

    CHECK(writer_stop(), "writer_stop reported a failure while draining");
    /* Stopped: synchronous again, and the result is the write's. */
    bool f[1] = {false};
    CHECK(persist_write_flags(path, f, 1), "synchronous write");
    CHECK(read_flag0(path, &got) == 0 && !got, "synchronous write not on disk");
    CHECK(!persist_write_flags(bad, t, 1), "synchronous failure not reported");
    remove(path);
    return 0;
}

/* --- Login worker (phase 4b) ---------------------------------------- */

static LoginJob *new_job(const char *user, const char *password) {
    LoginJob *j = calloc(1, sizeof(*j));
    if (!j) {
        return NULL;
    }
    snprintf(j->ml.username, sizeof(j->ml.username), "%s", user);
    snprintf(j->ml.password, sizeof(j->ml.password), "%s", password);
    if (!state_path(j->save_path, sizeof(j->save_path), "saves/%s.save", user)) {
        free(j);
        return NULL;
    }
    return j;
}

static bool all_zero(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i]) {
            return false;
        }
    }
    return true;
}

static int test_login_job_outcomes(void) {
    /* New character: a fresh Argon2id hash of the password. */
    LoginJob *j = new_job("newbie", "pw-newbie");
    CHECK(j, "alloc");
    login_job_run(j);
    CHECK(j->outcome == LOGIN_NEW, "missing save: outcome %d", j->outcome);
    CHECK(auth_verify_password(j->new_hash, "pw-newbie"), "new hash does not verify");
    CHECK(all_zero(j->ml.password, sizeof(j->ml.password)), "password not wiped");
    login_job_free(j);

    /* Existing character: right and wrong password. */
    Client *c = fx_player(0, "veteran");
    CHECK(c != NULL, "socketpair failed");
    c->level = 7;
    save_player_data(c);
    j = new_job("veteran", "pw-veteran");
    login_job_run(j);
    CHECK(j->outcome == LOGIN_LOADED && j->sd.level == 7, "right password: outcome %d level %d",
          j->outcome, j->sd.level);
    login_job_free(j);
    j = new_job("veteran", "nope");
    login_job_run(j);
    CHECK(j->outcome == LOGIN_BAD_PASSWORD, "wrong password: outcome %d", j->outcome);
    CHECK(all_zero(&j->sd, sizeof(j->sd)), "save image kept after a wrong password");
    login_job_free(j);

    /* Unreadable save: refused, file untouched. */
    char path[512];
    CHECK(state_path(path, sizeof(path), "saves/broken.save"), "state path");
    FILE *f = fopen(path, "wb");
    CHECK(f, "create broken save");
    fputs("garbage", f);
    fclose(f);
    j = new_job("broken", "pw");
    login_job_run(j);
    CHECK(j->outcome == LOGIN_UNREADABLE && j->why[0], "corrupt save: outcome %d", j->outcome);
    login_job_free(j);
    f = fopen(path, "rb");
    char buf[16] = {0};
    CHECK(f && fread(buf, 1, sizeof(buf), f) == 7 && memcmp(buf, "garbage", 7) == 0,
          "corrupt save modified");
    fclose(f);

    /* DM account without a DM password configured. */
    j = new_job("dm", "whatever");
    j->is_dm = true;
    login_job_run(j);
    CHECK(j->outcome == LOGIN_BAD_DM, "DM without a DM password: outcome %d", j->outcome);
    login_job_free(j);
    return 0;
}

static int test_login_worker_async(void) {
    CHECK(login_worker_start(), "login_worker_start");
    int efd = login_worker_eventfd();
    CHECK(efd >= 0, "eventfd");
    CHECK(login_worker_take() == NULL, "result before any job");
    enum { N = 4 };
    for (int k = 0; k < N; k++) {
        char name[16];
        snprintf(name, sizeof(name), "async%d", k);
        LoginJob *j = new_job(name, "pw-async");
        CHECK(j, "alloc");
        j->slot = k;
        j->gen = 100 + (uint64_t)k;
        login_worker_submit(j);
    }
    int seen = 0;
    while (seen < N) {
        struct pollfd p = {.fd = efd, .events = POLLIN};
        CHECK(poll(&p, 1, 20000) == 1, "no completion signalled (seen %d)", seen);
        LoginJob *j;
        while ((j = login_worker_take()) != NULL) {
            CHECK(j->gen == 100 + (uint64_t)j->slot, "job/slot mismatch");
            CHECK(j->outcome == LOGIN_NEW, "outcome %d", j->outcome);
            login_job_free(j);
            seen++;
        }
    }
    struct pollfd p = {.fd = efd, .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 0, "eventfd still readable with an empty queue");
    login_worker_stop();
    return 0;
}

static bool output_has(const char *buf, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(buf + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

/* --- Command table ---------------------------------------------------- */

/* Every command of the table answers, prefixes match as before, and the
 * DM commands exist only for the dm account. */
static int test_command_table(void) {
    Client *c = fx_player(0, "tabler");
    CHECK(c != NULL, "socketpair failed");
    static char buf[1 << 16];
    static const char *const known[] = {"help",   "?",      "stats",  "status", "inventory", "i",
                                        "spells", "who",    "party",  "event",  "look",      "l",
                                        "/look",  "search", "blocks", "save",   "rest"};
    for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        fx_drain(0);
        handle_text_cmd(c, known[k], g_npcs);
        size_t n = fx_output(0, buf, sizeof(buf));
        CHECK(n > 0, "'%s': no answer", known[k]);
        CHECK(!output_has(buf, n, "Unrecognized command"), "'%s' not recognized", known[k]);
    }
    static const char *const unknown[] = {"xyzzy", "helpme", "dm_gold 100", "", "stat"};
    for (size_t k = 0; k < sizeof(unknown) / sizeof(unknown[0]); k++) {
        fx_drain(0);
        handle_text_cmd(c, unknown[k], g_npcs);
        size_t n = fx_output(0, buf, sizeof(buf));
        CHECK(output_has(buf, n, "Unrecognized command"), "'%s' was accepted", unknown[k]);
    }
    /* dm_* runs only for the DM, and does not hide the player commands. */
    uint64_t gold = c->gold;
    c->is_dm = true;
    cmd(c, "dm_gold 100");
    CHECK(c->gold == gold + 100, "dm_gold: gold %llu", (unsigned long long)c->gold);
    fx_drain(0);
    handle_text_cmd(c, "who", g_npcs);
    size_t n = fx_output(0, buf, sizeof(buf));
    CHECK(output_has(buf, n, "[WHO]"), "'who' not reached by the DM");
    fx_drain(0);
    handle_text_cmd(c, "dm_goto", g_npcs);
    n = fx_output(0, buf, sizeof(buf));
    CHECK(output_has(buf, n, "Usage: dm_goto"), "dm_goto without arguments: no usage");
    c->is_dm = false;
    fx_drain(0);
    handle_text_cmd(c, "dm_goto", g_npcs);
    n = fx_output(0, buf, sizeof(buf));
    CHECK(output_has(buf, n, "Unrecognized command"), "dm_goto known to a player");
    return 0;
}

static bool answer_has(Client *c, const char *text, const char *needle) {
    static char buf[1 << 16];
    fx_drain(0);
    handle_text_cmd(c, text, g_npcs);
    size_t n = fx_output(0, buf, sizeof(buf));
    return output_has(buf, n, needle);
}

/* Prefixes match whole words; a known command with missing arguments
 * answers with its usage; train and pray do what help says. */
static int test_command_words(void) {
    size_t rows = 0;
    const CmdEntry *t = cmd_commands(&rows);
    for (size_t i = 0; i < rows; i++) {
        const char *p = t[i].prefixes;
        while (p) {
            const char *bar = strchr(p, '|');
            size_t len = bar ? (size_t)(bar - p) : strlen(p);
            CHECK(len > 0 && p[len - 1] == ' ', "row %zu: prefix without a trailing space", i);
            p = bar ? bar + 1 : NULL;
        }
    }
    Client *c = fx_player(0, "worder");
    CHECK(c != NULL, "socketpair failed");
    static const char *const words[] = {"trainer", "prayer",    "useless", "getaway", "lorem",
                                        "repairs", "identifyx", "unbeltx", "studying"};
    for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
        CHECK(answer_has(c, words[k], "Unrecognized command"), "'%s' was accepted", words[k]);
    }
    CHECK(answer_has(c, "buy", "Usage: buy"), "'buy': no usage");
    CHECK(answer_has(c, "cast", "Usage: cast"), "'cast': no usage");
    CHECK(answer_has(c, "talk", "no merchant"), "'talk' far from merchants");

    /* train: every ability, by abbreviation or full name, and nothing else. */
    c->unspent_stat_points = 3;
    int con = c->con, wis = c->wis;
    CHECK(answer_has(c, "train con", "Constitution"), "train con");
    CHECK(c->con == con + 1 && c->unspent_stat_points == 2, "train con: con %d", c->con);
    CHECK(answer_has(c, "train Wisdom ", "Wisdom"), "train Wisdom");
    CHECK(c->wis == wis + 1, "train Wisdom: wis %d", c->wis);
    CHECK(answer_has(c, "train strawberry", "Unknown stat"), "train strawberry");
    CHECK(c->unspent_stat_points == 1, "a wrong stat spent a point");

    /* pray: the prayer (pray <book> is the cleric ritual, tested with the books). */
    c->xp = 150;
    c->hp = 1;
    CHECK(answer_has(c, "pray", "You pray hard"), "pray");
    CHECK(c->xp == 50 && c->hp > 1, "pray: xp %d hp %d", c->xp, c->hp);
    CHECK(answer_has(c, "p", "The Gods do not respond"), "p without XP");
    return 0;
}

/* "open <dir>" and "close <dir>": the direction used to be unreachable
 * (only the bare words matched, and they ask for a direction). */
static int test_doors(void) {
    Client *c = fx_player(0, "doorman");
    CHECK(c != NULL, "socketpair failed");
    c->floor_id = 2;
    c->x = 60;
    c->y = 60;
    Map *m = &master_world->floors[2].map;
    m->data[0][60][61] = VOXEL_DOOR;
    cmd(c, "open e");
    CHECK(m->data[0][60][61] == VOXEL_FLOOR, "open e: tile %d", m->data[0][60][61]);
    cmd(c, "close e");
    CHECK(m->data[0][60][61] == VOXEL_DOOR, "close e: tile %d", m->data[0][60][61]);
    cmd(c, "o e");
    CHECK(m->data[0][60][61] == VOXEL_FLOOR, "o e: tile %d", m->data[0][60][61]);
    m->data[0][59][60] = VOXEL_FLOOR; /* never a door: stays floor */
    cmd(c, "close n");
    CHECK(m->data[0][59][60] == VOXEL_FLOOR, "closed a tile that was never a door");
    return 0;
}

static int item_named(const char *name) {
    for (int i = 0; i < item_database_size; i++) {
        if (strcmp(item_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int spell_named(const char *name) {
    for (int i = 0; i < spell_database_size; i++) {
        if (strcmp(spell_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* The class transit cantrips are learned from their book, in the class
 * temple, and cast only with the book at hand, like every other spell. */
static int test_transit_cantrips_need_a_book(void) {
    static const char *const cantrips[] = {
        "Ancestral Totem Call", "The Last Refrain", "Divine Recall",     "The Green Path",
        "Warrior's March",      "Flowing Step",     "Oathbound Passage", "Hunter's Trail",
        "The Back Door",        "Bloodline Rift",   "Pact Passage",      "Arcane Transit"};
    for (size_t k = 0; k < sizeof(cantrips) / sizeof(cantrips[0]); k++) {
        int si = spell_named(cantrips[k]);
        CHECK(si >= 0, "spell '%s' missing", cantrips[k]);
        int books = 0;
        for (int i = 0; i < item_database_size; i++) {
            const ItemTemplate *t = &item_database[i];
            if (t->category != ITEM_BOOK || !(t->book_class_mask & spell_database[si].class_mask)) {
                continue;
            }
            for (int b = 0; b < t->book_spell_count; b++) {
                books += strcmp(t->book_spell_names[b], cantrips[k]) == 0;
            }
        }
        CHECK(books == 1, "'%s' is in %d books of its class", cantrips[k], books);
    }
    /* The book shops of the city sell every travel tome. */
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    int next = 1;
    spawn_city_merchants(g_npcs, &next);
    spawn_magic_shops(g_npcs, &next);
    spawn_martial_archive(g_npcs, &next);
    for (size_t k = 0; k < sizeof(cantrips) / sizeof(cantrips[0]); k++) {
        bool sold = false;
        for (int i = 0; i < MAX_NPCS && !sold; i++) {
            const NPC *m = &g_npcs[i];
            if (!m->active || m->archetype != ARCH_MERCHANT) {
                continue;
            }
            for (int s = 0; s < m->merchant.item_count && !sold; s++) {
                const ItemTemplate *t = &item_database[m->merchant.item_templates[s]];
                sold = t->category == ITEM_BOOK && t->book_spell_count == 1 &&
                       strcmp(t->book_spell_names[0], cantrips[k]) == 0;
            }
        }
        CHECK(sold, "no shop sells the book of '%s'", cantrips[k]);
    }
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);

    Client *c = fx_player(0, "wayfarer");
    CHECK(c != NULL, "socketpair failed");
    c->class_id = CLASS_WIZARD;
    int si = spell_named("Arcane Transit");
    int book = item_named("Primer of Arcane Transit");
    CHECK(book >= 0, "no book for Arcane Transit");
    CHECK(!spell_known_get(c, si), "known before the book");
    static char buf[1 << 16];

    /* Learning: only inside the wizard temple on floor 0. */
    give_item(c, book);
    cmd(c, "study 1");
    CHECK(!spell_known_get(c, si), "learned outside the temple");
    const TempleInfo *tw = NULL;
    for (int i = 0; i < TEMPLE_COUNT; i++) {
        if (TEMPLES[i].cls == CLASS_WIZARD) {
            tw = &TEMPLES[i];
        }
    }
    CHECK(tw != NULL, "no wizard temple");
    c->floor_id = 0;
    c->x = (tw->x0 + tw->x1) / 2;
    c->y = (tw->y0 + tw->y1) / 2;
    cmd(c, "study 1");
    CHECK(spell_known_get(c, si), "not learned in the temple");

    /* Casting: refused without the book. */
    c->backpack_count = 0;
    fx_reset_item(&c->backpack[0]);
    fx_drain(0);
    handle_text_cmd(c, "cast Arcane Transit", g_npcs);
    size_t n = fx_output(0, buf, sizeof(buf));
    CHECK(output_has(buf, n, "the book containing it is not in your inventory"),
          "cast without the book was not refused");
    give_item(c, book);
    fx_drain(0);
    handle_text_cmd(c, "cast Arcane Transit", g_npcs);
    n = fx_output(0, buf, sizeof(buf));
    CHECK(!output_has(buf, n, "the book containing it is not in your inventory"),
          "cast with the book was refused");
    return 0;
}

/* --- Floor walk and entity lookup ---------------------------------------- */

static int scan_floor(int f, int *out) {
    int n = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (g_npcs[i].active && g_npcs[i].floor_id == f) {
            out[n++] = i;
        }
    }
    return n;
}

static int walk_floor(int f, int *out) {
    int n = 0;
    FloorIter it = floor_iter(g_npcs, f);
    for (int i; (i = floor_iter_next(&it)) >= 0;) {
        out[n++] = i;
    }
    return n;
}

static int compare_all_floors(const char *when) {
    static int a[MAX_NPCS], b[MAX_NPCS];
    for (int f = -1; f <= MAX_FLOORS; f++) {
        int na = scan_floor(f, a), nb = walk_floor(f, b);
        CHECK(na == nb, "%s: floor %d: scan %d NPCs, walk %d", when, f, na, nb);
        CHECK(memcmp(a, b, (size_t)na * sizeof(int)) == 0, "%s: floor %d: order differs", when, f);
    }
    return 0;
}

static void spawn_at(int slot, int floor_id, int entity_id) {
    NPC *n = &g_npcs[slot];
    n->floor_id = floor_id;
    n->entity_id = entity_id;
    n->respawn_timer = -1;
    npc_set_active(n, true);
}

/* A walk of the per-floor index returns what a scan of every slot returns,
 * in the same order: after a rebuild, after spawns and deaths since it,
 * for NPCs spawned during the walk, and for a floor over the index cap. */
static int test_floor_walk_matches_scan(void) {
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    srand(7);
    int next = 1;
    for (int k = 0; k < 6000; k++) {
        int s = rand() % MAX_NPCS;
        if (!g_npcs[s].active) {
            spawn_at(s, rand() % MAX_FLOORS, next++);
        }
    }
    if (compare_all_floors("spawned before any rebuild")) {
        return 1;
    }
    floor_index_rebuild(g_npcs);
    if (compare_all_floors("after the rebuild")) {
        return 1;
    }
    for (int k = 0; k < 4000; k++) {
        int s = rand() % MAX_NPCS;
        if (g_npcs[s].active && rand() % 2) {
            npc_set_active(&g_npcs[s], false);
        } else if (!g_npcs[s].active) {
            spawn_at(s, rand() % MAX_FLOORS, next++);
        }
    }
    if (compare_all_floors("spawns and deaths since the rebuild")) {
        return 1;
    }

    /* Spawned during the walk: a higher slot is met, a lower one is not. */
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    spawn_at(100, 3, next++);
    spawn_at(300, 3, next++);
    floor_index_rebuild(g_npcs);
    FloorIter it = floor_iter(g_npcs, 3);
    int first = floor_iter_next(&it);
    CHECK(first == 100, "first slot %d", first);
    spawn_at(50, 3, next++);
    spawn_at(200, 3, next++);
    int seen[4], n = 0;
    for (int i; (i = floor_iter_next(&it)) >= 0 && n < 4;) {
        seen[n++] = i;
    }
    CHECK(n == 2 && seen[0] == 200 && seen[1] == 300, "walk after spawns: %d slots", n);

    /* More NPCs on one floor than the index holds. */
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int s = 0; s < 5000; s++) {
        spawn_at(s * 7 % MAX_NPCS, 5, next++);
    }
    if (compare_all_floors("overflowing floor, before the rebuild")) {
        return 1;
    }
    floor_index_rebuild(g_npcs);
    if (compare_all_floors("overflowing floor, after the rebuild")) {
        return 1;
    }
    return 0;
}

static int test_entity_lookup(void) {
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int s = 0; s < 3000; s++) {
        spawn_at(s * 13 % MAX_NPCS, s % MAX_FLOORS, 10 + s);
    }
    floor_index_rebuild(g_npcs);
    for (int s = 0; s < 3000; s++) {
        NPC *n = npc_by_entity_id(g_npcs, 10 + s);
        CHECK(n == &g_npcs[s * 13 % MAX_NPCS], "entity %d not found", 10 + s);
    }
    CHECK(npc_by_entity_id(g_npcs, 0) == NULL, "id 0 found");
    CHECK(npc_by_entity_id(g_npcs, 999999) == NULL, "unknown id found");
    /* Dead since the rebuild, and a slot reused by another entity. */
    npc_set_active(&g_npcs[13], false);
    CHECK(npc_by_entity_id(g_npcs, 11) == NULL, "dead NPC found");
    spawn_at(13, 0, 777777);
    CHECK(npc_by_entity_id(g_npcs, 777777) == &g_npcs[13], "fresh NPC not found");
    CHECK(npc_by_entity_id(g_npcs, 11) == NULL, "old id of a reused slot found");
    return 0;
}

/* --- Movement ------------------------------------------------------------- */

static void open_ground(int floor_id, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            master_world->floors[floor_id].map.data[0][y][x] = VOXEL_FLOOR;
        }
    }
}

/* A step walks onto open ground, stops at a wall and at a merchant, and
 * picks up a gold pile on the way. */
static int test_player_move(void) {
    memset(g_npcs, 0, sizeof(NPC) * MAX_NPCS);
    Client *c = fx_player(0, "walker");
    CHECK(c != NULL, "socketpair failed");
    c->floor_id = 1; /* no city doors, no fountain */
    c->x = 50;
    c->y = 50;
    open_ground(1, 48, 48, 53, 52);
    Floor *fl = &master_world->floors[1];
    fl->entity_grid[50][50] = c->entity_id;
    MsgMove east = {.dx = 1, .dy = 0}, west = {.dx = -5, .dy = 0};
    static char buf[1 << 16];

    c->last_action_ms = 0;
    player_move(c, &east, g_test_clients, g_npcs);
    CHECK(c->x == 51 && c->y == 50, "step east: at %d,%d", c->x, c->y);
    CHECK(fl->entity_grid[50][50] == 0 && fl->entity_grid[50][51] == c->entity_id,
          "entity grid not updated");
    player_move(c, &east, g_test_clients, g_npcs); /* within 200 ms */
    CHECK(c->x == 51, "rate limit: moved to %d", c->x);

    NPC *gold = &g_npcs[42];
    gold->archetype = ARCH_GOLD;
    gold->gold_drop = 25;
    gold->x = 52;
    gold->y = 50;
    spawn_at(42, 1, 5000);
    fl->entity_grid[50][52] = gold->entity_id;
    uint64_t before = c->gold;
    c->last_action_ms = 0;
    player_move(c, &east, g_test_clients, g_npcs);
    CHECK(c->gold == before + 25, "gold %llu", (unsigned long long)c->gold);
    CHECK(!gold->active, "gold pile still there");
    CHECK(c->x == 52, "picking up gold does not block: at %d", c->x);

    NPC *m = &g_npcs[43];
    m->archetype = ARCH_MERCHANT;
    snprintf(m->merchant.shop_name, sizeof(m->merchant.shop_name), "Test Shop");
    m->x = 53;
    m->y = 50;
    spawn_at(43, 1, 5001);
    fl->entity_grid[50][53] = m->entity_id;
    fx_drain(0);
    c->last_action_ms = 0;
    player_move(c, &east, g_test_clients, g_npcs);
    size_t n = fx_output(0, buf, sizeof(buf));
    CHECK(c->x == 52, "walked into the merchant: at %d", c->x);
    CHECK(output_has(buf, n, "[Test Shop] Welcome!"), "no greeting");

    c->x = 48;
    fl->entity_grid[50][48] = c->entity_id;
    c->last_action_ms = 0;
    player_move(c, &west, g_test_clients, g_npcs); /* dx clamped to -1: rock at 47 */
    CHECK(c->x == 48, "walked into rock: at %d", c->x);
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
    {"writer_queue", test_writer_queue},
    {"login_job_outcomes", test_login_job_outcomes},
    {"login_worker_async", test_login_worker_async},
    {"command_table", test_command_table},
    {"command_words", test_command_words},
    {"doors", test_doors},
    {"transit_cantrips_need_a_book", test_transit_cantrips_need_a_book},
    {"floor_walk_matches_scan", test_floor_walk_matches_scan},
    {"entity_lookup", test_entity_lookup},
    {"player_move", test_player_move},
    {"spell_attack", test_spell_attack},
    {"spell_save_area", test_spell_save_area},
    {"spell_cone_and_line", test_spell_cone_and_line},
    {"spell_conditions", test_spell_conditions},
    {"spell_self", test_spell_self},
    {"spell_cloud", test_spell_cloud},
    {"spell_special_and_terrain", test_spell_special_and_terrain},
    {"spell_refusals", test_spell_refusals},
    {"spell_every_effect", test_spell_every_effect},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <test>\n", argv[0]);
        for (size_t i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++) {
            fprintf(stderr, "  %s\n", k_tests[i].name);
        }
        return 2;
    }
    char state_dir[160]; /* one per test: each process locks its own */
    snprintf(state_dir, sizeof(state_dir), "state-%s", argv[1]);
    if (fx_init(state_dir, DGL_TEST_DATA_DIR) != 0) {
        return 1;
    }
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
