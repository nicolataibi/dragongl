/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Fuzz target: the save-file readers (persist.c, binio.c).
 *
 * The first input byte selects the reader (low 3 bits: player, NPCs,
 * tombstone, flags, world). The rest is a payload: it is wrapped in a
 * valid container (magic, version, length, CRC-32) so the fuzzer reaches
 * the field decoders instead of stopping at the CRC. With bit 7 set, the
 * rest is written as the whole file instead, to exercise the container
 * checks themselves. A reader must reject anything malformed and never
 * read or write out of bounds.
 */

#include "fuzz_common.h"
#include "fixture.h"

#include "binio.h"
#include "persist.h"
#include "server_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_PLAYER, KIND_NPCS, KIND_TOMB, KIND_FLAGS, KIND_WORLD, KIND_COUNT };

#define FUZZ_NPCS  256
#define FUZZ_FLAGS 58 /* MAX_ARTIFACTS in main_server.c */

static const struct {
    uint32_t magic;
    uint16_t version;
} k_formats[KIND_COUNT] = {
    {PERSIST_SAVE_MAGIC, PERSIST_SAVE_VERSION},   {PERSIST_NPCS_MAGIC, PERSIST_NPCS_VERSION},
    {PERSIST_TOMB_MAGIC, PERSIST_TOMB_VERSION},   {PERSIST_ART_MAGIC, PERSIST_ART_VERSION},
    {PERSIST_WORLD_MAGIC, PERSIST_WORLD_VERSION},
};

static char g_path[512];

static void put_le(uint8_t *p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static bool write_file(int kind, bool raw, const uint8_t *p, size_t n) {
    FILE *f = fopen(g_path, "wb");
    if (!f) {
        return false;
    }
    bool ok = true;
    if (!raw) {
        uint8_t h[BINFILE_HEADER_SIZE] = {0};
        put_le(h, k_formats[kind].magic, 4);
        put_le(h + 4, k_formats[kind].version, 2);
        put_le(h + 8, (uint32_t)n, 4);
        put_le(h + 12, crc32_data(p, n), 4);
        ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    }
    ok = ok && (n == 0 || fwrite(p, 1, n, f) == n);
    return fclose(f) == 0 && ok;
}

static void read_back(int kind) {
    static SaveData sd;
    static NPC npcs[FUZZ_NPCS];
    static Tombstone tomb;
    bool flags[FUZZ_FLAGS];
    char why[128];
    int next_id = 0, turns = 0, loaded = 0;
    switch (kind) {
    case KIND_PLAYER:
        (void)persist_read_player(g_path, &sd, why, sizeof(why));
        break;
    case KIND_NPCS:
        memset(npcs, 0, sizeof(npcs));
        (void)persist_read_npcs(g_path, npcs, FUZZ_NPCS, &next_id, &turns, &loaded, why,
                                sizeof(why));
        break;
    case KIND_TOMB:
        (void)persist_read_tombstone(g_path, &tomb, why, sizeof(why));
        break;
    case KIND_FLAGS:
        (void)persist_read_flags(g_path, flags, FUZZ_FLAGS, why, sizeof(why));
        break;
    default:
        (void)persist_read_world(g_path, master_world, why, sizeof(why));
        break;
    }
}

/* --- Seeds: the payload of files written by the real writers ----------- */

static void seed_from_file(const char *dir, const char *name, int kind) {
    FILE *f = fopen(g_path, "rb");
    if (!f) {
        perror(g_path);
        exit(1);
    }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < BINFILE_HEADER_SIZE) {
        fprintf(stderr, "%s: short file\n", name);
        exit(1);
    }
    fuzz_write_seed(dir, name, kind, buf + BINFILE_HEADER_SIZE, n - BINFILE_HEADER_SIZE);
}

static void write_seeds(const char *dir) {
    static SaveData sd;
    for (int i = 0; i < MAX_BACKPACK; i++) {
        fx_reset_item(&sd.backpack[i]);
    }
    sd.backpack[0].template_idx = 0;
    sd.backpack[0].stack_count = 1;
    sd.backpack_count = 1;
    sd.level = 3;
    sd.hp = sd.max_hp = 25;
    sd.effect_count = 1;
    sd.effects[0].cond = 0;
    sd.effects[0].duration_rounds = 3;
    sd.blocked_count = 1;
    strcpy(sd.blocked_players[0], "mallory");
    if (!persist_write_player(g_path, &sd)) {
        exit(1);
    }
    seed_from_file(dir, "player", KIND_PLAYER);

    static NPC npcs[FUZZ_NPCS];
    for (int i = 0; i < 3; i++) {
        npcs[i].active = true;
        npcs[i].template_idx = i;
        npcs[i].hp = 10;
        npcs[i].respawn_timer = -1;
    }
    if (!persist_write_npcs(g_path, npcs, FUZZ_NPCS, 4, 100)) {
        exit(1);
    }
    seed_from_file(dir, "npcs", KIND_NPCS);

    static Tombstone tomb;
    tomb.backpack_count = 0;
    if (!persist_write_tombstone(g_path, &tomb)) {
        exit(1);
    }
    seed_from_file(dir, "tombstone", KIND_TOMB);

    bool flags[FUZZ_FLAGS] = {true, false, true};
    if (!persist_write_flags(g_path, flags, FUZZ_FLAGS)) {
        exit(1);
    }
    seed_from_file(dir, "flags", KIND_FLAGS);
}

/* The signature is libFuzzer's: argc and argv may be changed by the target. */
// NOLINTNEXTLINE(readability-non-const-parameter)
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    char state[300];
    snprintf(state, sizeof(state), "%s/state", fuzz_tmp_dir());
    if (fx_init(state, DGL_FUZZ_DATA_DIR) != 0) {
        exit(1);
    }
    snprintf(g_path, sizeof(g_path), "%s/input.dat", fuzz_tmp_dir());
    const char *dir = fuzz_seed_dir();
    if (dir) {
        write_seeds(dir);
        exit(0);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) {
        return 0;
    }
    int kind = (data[0] & 7) % KIND_COUNT;
    bool raw = (data[0] & 0x80) != 0;
    if (write_file(kind, raw, data + 1, size - 1)) {
        read_back(kind);
    }
    return 0;
}
