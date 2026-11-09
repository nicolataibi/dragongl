/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Fuzz target: the wire decoders (wire.c).
 *
 * An input is one frame as it arrives from a peer: the 8-byte header, then
 * the payload. The header is decoded, the payload limits of both
 * directions are queried, and the payload goes to the decoder of the type
 * named by the header, as the server (client messages) and the client
 * (server messages) do. Decoders must reject any malformed payload
 * without reading past it.
 */

#include "fuzz_common.h"

#include "binio.h"
#include "protocol.h"
#include "wire.h"

#include <stdlib.h>
#include <string.h>

static void decode(MsgType type, const uint8_t *p, size_t n) {
    union {
        MsgLogin login;
        MsgMove move;
        MsgTextCmd text_cmd;
        MsgWelcome welcome;
        MsgState state;
        MsgMapChunk chunk;
        MsgText text;
        MsgAuthFail auth_fail;
        MsgSpellVFX vfx;
        MsgTombstoneRemove tomb;
        MsgTimeSync sync;
        MsgCombatEvent combat;
    } m;
    static EntityUpdateRec recs[WIRE_MAX_ENTITY_RECS];
    const uint8_t *tiles = NULL;
    int count = 0;
    memset(&m, 0, sizeof(m));
    switch (type) {
    case MSG_LOGIN:
        (void)wire_get_login(p, n, &m.login);
        break;
    case MSG_MOVE:
        (void)wire_get_move(p, n, &m.move);
        break;
    case MSG_TEXT_CMD:
        (void)wire_get_text_cmd(p, n, &m.text_cmd);
        break;
    case MSG_WELCOME:
        (void)wire_get_welcome(p, n, &m.welcome);
        break;
    case MSG_STATE:
        (void)wire_get_state(p, n, &m.state);
        break;
    case MSG_MAP_CHUNK:
        if (wire_get_map_chunk(p, n, &m.chunk, &tiles)) {
            /* The tiles stay in the payload: touch every one of them. */
            size_t cells = (size_t)m.chunk.width * (size_t)m.chunk.height;
            volatile uint8_t sum = 0;
            for (size_t i = 0; i < cells; i++) {
                sum = (uint8_t)(sum + tiles[i]);
            }
            (void)sum;
        }
        break;
    case MSG_TEXT:
        (void)wire_get_text(p, n, &m.text);
        break;
    case MSG_AUTH_FAIL:
        (void)wire_get_auth_fail(p, n, &m.auth_fail);
        break;
    case MSG_SPELL_VFX:
        (void)wire_get_spell_vfx(p, n, &m.vfx);
        break;
    case MSG_TOMBSTONE_REMOVE:
        (void)wire_get_tombstone_remove(p, n, &m.tomb);
        break;
    case MSG_TIME_SYNC:
        (void)wire_get_time_sync(p, n, &m.sync);
        break;
    case MSG_ENTITY_UPDATE:
        (void)wire_get_entity_update(p, n, recs, WIRE_MAX_ENTITY_RECS, &count);
        break;
    case MSG_COMBAT_EVENT:
        (void)wire_get_combat_event(p, n, &m.combat);
        break;
    default:
        break;
    }
}

static void seed(const char *dir, const char *name, BinWriter *w) {
    fuzz_write_seed(dir, name, -1, w->data, w->len);
    bw_reset(w);
}

static void write_seeds(const char *dir) {
    BinWriter w;
    bw_init(&w);
    MsgLogin login;
    memset(&login, 0, sizeof(login));
    strcpy(login.username, "alice");
    strcpy(login.password, "secret");
    strcpy(login.server_pass, "gate");
    (void)wire_put_login(&w, &login);
    seed(dir, "login", &w);
    MsgMove move = {1, -1};
    (void)wire_put_move(&w, &move);
    seed(dir, "move", &w);
    (void)wire_put_text_cmd(&w, "cast 1 2");
    seed(dir, "text_cmd", &w);
    MsgWelcome welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.entity_id = 7;
    welcome.hp = welcome.max_hp = 20;
    (void)wire_put_welcome(&w, &welcome);
    seed(dir, "welcome", &w);
    static MsgState state;
    strcpy(state.username, "alice");
    strcpy(state.weapon_name, "Dagger");
    (void)wire_put_state(&w, &state);
    seed(dir, "state", &w);
    MsgMapChunk chunk = {10, 20, 4, 3};
    uint8_t tiles[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    (void)wire_put_map_chunk(&w, &chunk, tiles);
    seed(dir, "map_chunk", &w);
    (void)wire_put_text(&w, "[WHO] alice");
    seed(dir, "text", &w);
    (void)wire_put_auth_fail(&w, "wrong password");
    seed(dir, "auth_fail", &w);
    MsgSpellVFX vfx;
    memset(&vfx, 0, sizeof(vfx));
    (void)wire_put_spell_vfx(&w, &vfx);
    seed(dir, "spell_vfx", &w);
    (void)wire_put_tombstone_remove(&w, 42);
    seed(dir, "tombstone_remove", &w);
    MsgTimeSync sync;
    memset(&sync, 0, sizeof(sync));
    (void)wire_put_time_sync(&w, &sync);
    seed(dir, "time_sync", &w);
    EntityUpdateRec recs[2] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    (void)wire_put_entity_update(&w, recs, 2);
    seed(dir, "entity_update", &w);
    MsgCombatEvent combat;
    memset(&combat, 0, sizeof(combat));
    combat.kind = COMBAT_EV_HEAL;
    combat.amount = 5;
    (void)wire_put_combat_event(&w, &combat);
    seed(dir, "combat_event", &w);
    bw_free(&w);
}

/* The signature is libFuzzer's: argc and argv may be changed by the target. */
// NOLINTNEXTLINE(readability-non-const-parameter)
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    const char *dir = fuzz_seed_dir();
    if (dir) {
        write_seeds(dir);
        exit(0);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < WIRE_HEADER_SIZE) {
        return 0;
    }
    MsgHeader h;
    wire_read_header(data, &h);
    (void)wire_client_payload_max(h.type);
    (void)wire_server_payload_max(h.type);
    decode(h.type, data + WIRE_HEADER_SIZE, size - WIRE_HEADER_SIZE);
    return 0;
}
