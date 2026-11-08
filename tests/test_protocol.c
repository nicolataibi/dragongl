/*
 * DRAGON GL - Unit tests: wire protocol v4 (include/wire.h)
 *
 * Every message is encoded and decoded back; the byte layout of the
 * header and of a few payloads is pinned down, and malformed payloads
 * (truncated, trailing bytes, oversized strings, out-of-range values)
 * must be refused. Run via `ctest` (see CMakeLists.txt).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wire.h"

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                              \
        }                                                              \
    } while (0)

/* Header of the single frame in w; returns its payload. */
static const uint8_t *frame(const BinWriter *w, MsgType type, uint32_t *len) {
    MsgHeader h;
    CHECK(w->len >= WIRE_HEADER_SIZE);
    wire_read_header(w->data, &h);
    CHECK(h.version == PROTOCOL_VERSION);
    CHECK(h.type == type);
    CHECK(h.length == w->len - WIRE_HEADER_SIZE);
    *len = h.length;
    return w->data + WIRE_HEADER_SIZE;
}

static void test_header_layout(void) {
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_tombstone_remove(&w, -3));
    /* u16 version, u16 type, u32 length, little-endian; then i32 id */
    const uint8_t expect[] = {PROTOCOL_VERSION, 0, MSG_TOMBSTONE_REMOVE, 0, 4, 0, 0, 0,
                              0xFD, 0xFF, 0xFF, 0xFF};
    CHECK(w.len == sizeof(expect));
    CHECK(memcmp(w.data, expect, sizeof(expect)) == 0);
    bw_free(&w);
    CHECK(WIRE_HEADER_SIZE == 8);
    CHECK(PROTOCOL_VERSION == 4);
}

static void test_login(void) {
    MsgLogin in, out;
    memset(&in, 0, sizeof(in));
    snprintf(in.username, sizeof(in.username), "Arathor_1");
    memset(in.password, 'p', PROTO_SECRET_LEN - 1); /* 63 characters fit */
    snprintf(in.server_pass, sizeof(in.server_pass), "gate-secret");
    in.race_id = 3;
    in.subrace_id = -1;
    in.class_id = 7;
    in.alignment = 4;
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_login(&w, &in));
    uint32_t n;
    const uint8_t *p = frame(&w, MSG_LOGIN, &n);
    CHECK(n <= wire_client_payload_max(MSG_LOGIN));
    CHECK(wire_get_login(p, n, &out));
    CHECK(strcmp(out.username, "Arathor_1") == 0);
    CHECK(strlen(out.password) == PROTO_SECRET_LEN - 1);
    CHECK(strcmp(out.server_pass, "gate-secret") == 0);
    CHECK(out.race_id == 3 && out.subrace_id == -1 && out.class_id == 7 && out.alignment == 4);
    /* truncated and padded payloads are refused */
    CHECK(!wire_get_login(p, n - 1, &out));
    uint8_t longer[512];
    memcpy(longer, p, n);
    longer[n] = 0;
    CHECK(!wire_get_login(longer, n + 1, &out));
    /* a username longer than its field is refused, not truncated */
    uint8_t bad[512];
    size_t k = 0;
    bad[k++] = 40;
    memset(bad + k, 'a', 40);
    k += 40;
    bad[k++] = 1;
    bad[k++] = 'x';
    bad[k++] = 1;
    bad[k++] = 'y';
    memset(bad + k, 0, 4);
    k += 4;
    CHECK(!wire_get_login(bad, k, &out));
    bw_free(&w);
}

static void test_move_and_text_cmd(void) {
    BinWriter w;
    bw_init(&w);
    MsgMove mv = {-1, 1}, back;
    CHECK(wire_put_move(&w, &mv));
    uint32_t n;
    const uint8_t *p = frame(&w, MSG_MOVE, &n);
    CHECK(n == 2);
    CHECK(wire_get_move(p, n, &back) && back.dx == -1 && back.dy == 1);
    const uint8_t far[2] = {2, 0};
    CHECK(!wire_get_move(far, 2, &back)); /* only one step at a time */

    bw_reset(&w);
    char cmd[400];
    memset(cmd, 'c', sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    CHECK(wire_put_text_cmd(&w, cmd)); /* cut to the field */
    p = frame(&w, MSG_TEXT_CMD, &n);
    CHECK(n == 1 + PROTO_TEXT_LEN - 1);
    CHECK(n <= wire_client_payload_max(MSG_TEXT_CMD));
    MsgTextCmd tc;
    CHECK(wire_get_text_cmd(p, n, &tc) && strlen(tc.cmd) == PROTO_TEXT_LEN - 1);
    CHECK(wire_client_payload_max(MSG_STATE) == 0); /* not a client message */
    CHECK(wire_client_payload_max(MSG_LOGIN) + WIRE_HEADER_SIZE <= WIRE_HEADER_SIZE + WIRE_MAX_CLIENT_PAYLOAD);
    bw_free(&w);
}

static void test_welcome_and_state(void) {
    BinWriter w;
    bw_init(&w);
    static MsgWelcome wi = {42, 150, 151, 1, 20, 25, 1ULL << 40, 2, -1, 5, 3, 6}, wo;
    CHECK(wire_put_welcome(&w, &wi));
    uint32_t n;
    const uint8_t *p = frame(&w, MSG_WELCOME, &n);
    CHECK(wire_get_welcome(p, n, &wo));
    CHECK(memcmp(&wi, &wo, sizeof(wi)) == 0);

    static MsgState si, so;
    memset(&si, 0, sizeof(si));
    si.gold = 123456789012ULL;
    si.entity_id = 7;
    si.x = 299;
    si.y = 0;
    si.hp = 1 << 20; /* boss HP beyond int16 */
    si.max_hp = 1 << 21;
    si.floor_id = 99;
    si.movement_cooldown = 0.35f;
    si.status_icons = 0x0FFFu;
    si.bosses_defeated = 0x3FFu;
    si.spell_slots[9] = 16;
    si.spell_slots_max[1] = 24;
    snprintf(si.weapon_name, sizeof(si.weapon_name), "Iron Sword");
    snprintf(si.eq_ring[9], sizeof(si.eq_ring[9]), "Copper Ring");
    snprintf(si.eq_belt[3], sizeof(si.eq_belt[3]), "Potion");
    si.is_merchant = 1;
    si.shop_spec = SHOP_SPEC_BOOKS_MARTIAL;
    si.is_player = 1;
    snprintf(si.username, sizeof(si.username), "hero");
    bw_reset(&w);
    CHECK(wire_put_state(&w, &si));
    p = frame(&w, MSG_STATE, &n);
    CHECK(n <= wire_server_payload_max(MSG_STATE));
    CHECK(wire_get_state(p, n, &so));
    CHECK(memcmp(&si, &so, sizeof(si)) == 0);
    CHECK(!wire_get_state(p, n - 1, &so));
    bw_free(&w);
}

static void test_map_chunk(void) {
    enum { W = 10, H = 4 };
    uint8_t tiles[W * H];
    for (int i = 0; i < W * H; i++)
        tiles[i] = (uint8_t)(i % VOXEL_COUNT);
    MsgMapChunk mc = {290, 296, W, H}, back;
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_map_chunk(&w, &mc, tiles));
    uint32_t n;
    const uint8_t *p = frame(&w, MSG_MAP_CHUNK, &n);
    CHECK(n == 8 + W * H);
    const uint8_t *t = NULL;
    CHECK(wire_get_map_chunk(p, n, &back, &t));
    CHECK(back.start_x == 290 && back.width == W && t && memcmp(t, tiles, W * H) == 0);
    /* a tile value past the voxel table, a rectangle outside the map */
    uint8_t copy[8 + W * H];
    memcpy(copy, p, n);
    copy[8 + 5] = VOXEL_COUNT;
    CHECK(!wire_get_map_chunk(copy, n, &back, &t));
    memcpy(copy, p, n);
    copy[0] = (uint8_t)295; /* start_x 295 + width 10 > MAP_WIDTH */
    CHECK(!wire_get_map_chunk(copy, n, &back, &t));
    MsgMapChunk huge = {0, 0, WIRE_MAX_CHUNK_SIDE + 1, 1};
    CHECK(!wire_put_map_chunk(&w, &huge, tiles));
    bw_free(&w);
}

static void test_entity_update(void) {
    EntityUpdateRec in[3] = {{1, 2, 3, 4}, {-5, 299, 0, 1 << 30}, {9, -1, -1, -7}}, out[3];
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_entity_update(&w, in, 3));
    uint32_t n;
    const uint8_t *p = frame(&w, MSG_ENTITY_UPDATE, &n);
    CHECK(n == 2 + 3 * WIRE_ENTITY_REC_SIZE);
    int count = 0;
    CHECK(wire_get_entity_update(p, n, out, 3, &count) && count == 3);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(!wire_get_entity_update(p, n, out, 2, &count));     /* more than the caller holds */
    CHECK(!wire_get_entity_update(p, n - 1, out, 3, &count)); /* count/length mismatch */
    CHECK(!wire_put_entity_update(&w, in, 0));
    bw_free(&w);
}

static void test_small_messages(void) {
    BinWriter w;
    bw_init(&w);
    uint32_t n;
    const uint8_t *p;

    CHECK(wire_put_text(&w, "[WHO] 1 player(s) online"));
    p = frame(&w, MSG_TEXT, &n);
    MsgText t;
    CHECK(wire_get_text(p, n, &t) && strcmp(t.text, "[WHO] 1 player(s) online") == 0);

    bw_reset(&w);
    CHECK(wire_put_auth_fail(&w, "Wrong server password."));
    p = frame(&w, MSG_AUTH_FAIL, &n);
    MsgAuthFail f;
    CHECK(wire_get_auth_fail(p, n, &f) && strcmp(f.reason, "Wrong server password.") == 0);

    bw_reset(&w);
    MsgSpellVFX v = {10, 11, 12, 13, 2, 1.0f, 0.5f, 0.0f}, vb;
    CHECK(wire_put_spell_vfx(&w, &v));
    p = frame(&w, MSG_SPELL_VFX, &n);
    CHECK(wire_get_spell_vfx(p, n, &vb));
    CHECK(vb.start_x == 10 && vb.target_y == 13 && vb.vfx_type == 2);
    CHECK(vb.color_r == 1.0f && vb.color_b == 0.0f && vb.color_g > 0.49f && vb.color_g < 0.51f);

    bw_reset(&w);
    MsgTimeSync ts = {23, 59, 123456}, tb;
    CHECK(wire_put_time_sync(&w, &ts));
    p = frame(&w, MSG_TIME_SYNC, &n);
    CHECK(wire_get_time_sync(p, n, &tb) && memcmp(&ts, &tb, sizeof(ts)) == 0);

    bw_reset(&w);
    MsgCombatEvent ev = {COMBAT_EV_CRITICAL, 77, 31}, eb;
    CHECK(wire_put_combat_event(&w, &ev));
    p = frame(&w, MSG_COMBAT_EVENT, &n);
    CHECK(wire_get_combat_event(p, n, &eb) && memcmp(&ev, &eb, sizeof(ev)) == 0);
    uint8_t bad[9];
    memcpy(bad, p, n);
    bad[0] = COMBAT_EV_MAX + 1;
    CHECK(!wire_get_combat_event(bad, n, &eb));
    MsgCombatEvent unknown = {0, 0, 0};
    CHECK(!wire_put_combat_event(&w, &unknown));

    bw_reset(&w);
    CHECK(wire_put_tombstone_remove(&w, -12));
    p = frame(&w, MSG_TOMBSTONE_REMOVE, &n);
    MsgTombstoneRemove rm;
    CHECK(wire_get_tombstone_remove(p, n, &rm) && rm.entity_id == -12);
    bw_free(&w);
}

static void test_several_frames_in_one_buffer(void) {
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_text(&w, "one"));
    CHECK(wire_put_text(&w, "two"));
    MsgHeader h;
    wire_read_header(w.data, &h);
    size_t second = WIRE_HEADER_SIZE + h.length;
    wire_read_header(w.data + second, &h);
    MsgText t;
    CHECK(h.type == MSG_TEXT &&
          wire_get_text(w.data + second + WIRE_HEADER_SIZE, h.length, &t) &&
          strcmp(t.text, "two") == 0);
    bw_free(&w);
}

int main(void) {
    test_header_layout();
    test_login();
    test_move_and_text_cmd();
    test_welcome_and_state();
    test_map_chunk();
    test_entity_update();
    test_small_messages();
    test_several_frames_in_one_buffer();
    if (g_failures == 0) {
        printf("test_protocol: all passed\n");
        return 0;
    }
    printf("test_protocol: %d FAILURES\n", g_failures);
    return 1;
}
