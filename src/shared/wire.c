/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "wire.h"

#include <string.h>

/* --- Helpers ----------------------------------------------------------------- */

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

void wire_read_header(const uint8_t *buf, MsgHeader *h) {
    h->version = (uint16_t)(buf[0] | (buf[1] << 8));
    h->type = (MsgType)(buf[2] | (buf[3] << 8));
    h->length = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
                ((uint32_t)buf[7] << 24);
}

uint32_t wire_client_payload_max(MsgType type) {
    switch (type) {
    case MSG_LOGIN:    return 4 + 3 + (PROTO_NAME_LEN - 1) + 2 * (PROTO_SECRET_LEN - 1);
    case MSG_MOVE:     return 2;
    case MSG_TEXT_CMD: return 1 + (PROTO_TEXT_LEN - 1);
    default:           return 0;
    }
}

uint32_t wire_server_payload_max(MsgType type) {
    switch (type) {
    case MSG_MAP_CHUNK:
        return 8 + WIRE_MAX_CHUNK_SIDE * WIRE_MAX_CHUNK_SIDE;
    case MSG_ENTITY_UPDATE:
        return 2 + WIRE_MAX_ENTITY_RECS * WIRE_ENTITY_REC_SIZE;
    case MSG_WELCOME:
    case MSG_STATE:
    case MSG_TEXT:
    case MSG_AUTH_FAIL:
    case MSG_SPELL_VFX:
    case MSG_TOMBSTONE_REMOVE:
    case MSG_TIME_SYNC:
    case MSG_COMBAT_EVENT:
        return 2048;
    default:
        return 0;
    }
}

/* Header with a zero length; frame_end() patches it. Returns the offset of
 * the header in the writer. */
static size_t frame_begin(BinWriter *w, MsgType type) {
    size_t at = w->len;
    bw_u16(w, PROTOCOL_VERSION);
    bw_u16(w, type);
    bw_u32(w, 0);
    return at;
}

static bool frame_end(BinWriter *w, size_t at) {
    if (w->failed)
        return false;
    size_t payload = w->len - at - WIRE_HEADER_SIZE;
    if (payload > WIRE_MAX_PAYLOAD) {
        w->len = at; /* drop the frame, keep what was there before */
        return false;
    }
    put_le32(w->data + at + 4, (uint32_t)payload);
    return true;
}

/* String: u8 length + bytes. Longer strings are cut to cap-1 bytes. */
static void put_str(BinWriter *w, const char *s, size_t cap) {
    size_t n = s ? strnlen(s, cap - 1) : 0;
    if (n > 255)
        n = 255;
    bw_u8(w, (uint8_t)n);
    bw_bytes(w, s, n);
}

static void get_str(BinReader *r, char *out, size_t cap) {
    size_t n = br_u8(r);
    if (n >= cap) {
        r->failed = true;
        out[0] = '\0';
        return;
    }
    br_bytes(r, out, n);
    out[n] = '\0';
    if (r->failed)
        out[0] = '\0';
}

static void put_i8(BinWriter *w, int v) {
    bw_u8(w, (uint8_t)(int8_t)v);
}

static int get_i8(BinReader *r) {
    return (int8_t)br_u8(r);
}

static void put_i16(BinWriter *w, int v) {
    bw_u16(w, (uint16_t)(int16_t)v);
}

static int get_i16(BinReader *r) {
    return (int16_t)br_u16(r);
}

static void put_f32(BinWriter *w, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    bw_u32(w, u);
}

static float get_f32(BinReader *r) {
    uint32_t u = br_u32(r);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* A colour channel 0..1 as a byte. */
static void put_unit(BinWriter *w, float f) {
    if (!(f > 0.0f))
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    bw_u8(w, (uint8_t)(f * 255.0f + 0.5f));
}

static float get_unit(BinReader *r) {
    return (float)br_u8(r) / 255.0f;
}

static void begin_read(BinReader *r, const void *p, size_t n, void *out, size_t out_size) {
    memset(out, 0, out_size);
    br_init(r, p, n);
}

/* --- Client -> server ------------------------------------------------------ */

bool wire_put_login(BinWriter *w, const MsgLogin *m) {
    size_t at = frame_begin(w, MSG_LOGIN);
    put_str(w, m->username, sizeof(m->username));
    put_str(w, m->password, sizeof(m->password));
    put_str(w, m->server_pass, sizeof(m->server_pass));
    put_i8(w, m->race_id);
    put_i8(w, m->subrace_id);
    put_i8(w, m->class_id);
    put_i8(w, m->alignment);
    return frame_end(w, at);
}

bool wire_get_login(const void *p, size_t n, MsgLogin *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    get_str(&r, m->username, sizeof(m->username));
    get_str(&r, m->password, sizeof(m->password));
    get_str(&r, m->server_pass, sizeof(m->server_pass));
    m->race_id = get_i8(&r);
    m->subrace_id = get_i8(&r);
    m->class_id = get_i8(&r);
    m->alignment = get_i8(&r);
    return br_done(&r);
}

bool wire_put_move(BinWriter *w, const MsgMove *m) {
    size_t at = frame_begin(w, MSG_MOVE);
    put_i8(w, m->dx);
    put_i8(w, m->dy);
    return frame_end(w, at);
}

bool wire_get_move(const void *p, size_t n, MsgMove *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->dx = get_i8(&r);
    m->dy = get_i8(&r);
    return br_done(&r) && m->dx >= -1 && m->dx <= 1 && m->dy >= -1 && m->dy <= 1;
}

bool wire_put_text_cmd(BinWriter *w, const char *cmd) {
    size_t at = frame_begin(w, MSG_TEXT_CMD);
    put_str(w, cmd, PROTO_TEXT_LEN);
    return frame_end(w, at);
}

bool wire_get_text_cmd(const void *p, size_t n, MsgTextCmd *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    get_str(&r, m->cmd, sizeof(m->cmd));
    return br_done(&r);
}

/* --- Server -> client ------------------------------------------------------ */

bool wire_put_welcome(BinWriter *w, const MsgWelcome *m) {
    size_t at = frame_begin(w, MSG_WELCOME);
    bw_i32(w, m->entity_id);
    bw_i32(w, m->x);
    bw_i32(w, m->y);
    bw_bool(w, m->is_new != 0);
    bw_i32(w, m->hp);
    bw_i32(w, m->max_hp);
    bw_u64(w, m->gold);
    put_i8(w, m->race_id);
    put_i8(w, m->subrace_id);
    put_i8(w, m->class_id);
    bw_i32(w, m->level);
    put_i8(w, m->alignment);
    return frame_end(w, at);
}

bool wire_get_welcome(const void *p, size_t n, MsgWelcome *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->entity_id = br_i32(&r);
    m->x = br_i32(&r);
    m->y = br_i32(&r);
    m->is_new = br_bool(&r) ? 1 : 0;
    m->hp = br_i32(&r);
    m->max_hp = br_i32(&r);
    m->gold = br_u64(&r);
    m->race_id = get_i8(&r);
    m->subrace_id = get_i8(&r);
    m->class_id = get_i8(&r);
    m->level = br_i32(&r);
    m->alignment = get_i8(&r);
    return br_done(&r);
}

bool wire_put_state(BinWriter *w, const MsgState *m) {
    size_t at = frame_begin(w, MSG_STATE);
    bw_u64(w, m->gold);
    bw_i32(w, m->entity_id);
    bw_i32(w, m->x);
    bw_i32(w, m->y);
    bw_i32(w, m->hp);
    bw_i32(w, m->max_hp);
    bw_i32(w, m->floor_id);
    bw_i32(w, m->game_hour);
    bw_i32(w, m->game_min);
    bw_i32(w, m->total_turns);
    bw_i32(w, m->str);
    bw_i32(w, m->dex);
    bw_i32(w, m->con);
    bw_i32(w, m->intel);
    bw_i32(w, m->wis);
    bw_i32(w, m->cha);
    put_f32(w, m->movement_cooldown);
    bw_i32(w, m->equipped_mask);
    bw_i32(w, m->level);
    bw_i32(w, m->xp);
    bw_i32(w, m->vision_radius);
    bw_i32(w, m->ac);
    bw_u32(w, m->status_icons);
    put_str(w, m->weapon_name, sizeof(m->weapon_name));
    put_str(w, m->armor_name, sizeof(m->armor_name));
    bw_i32(w, m->to_hit);
    bw_i32(w, m->to_dmg);
    bw_u32(w, m->bosses_defeated);
    bw_i32(w, m->hunger_level);
    for (int i = 0; i < 10; i++)
        bw_i32(w, m->spell_slots[i]);
    for (int i = 0; i < 10; i++)
        bw_i32(w, m->spell_slots_max[i]);
    put_str(w, m->eq_head, PROTO_NAME_LEN);
    put_str(w, m->eq_neck, PROTO_NAME_LEN);
    put_str(w, m->eq_body, PROTO_NAME_LEN);
    put_str(w, m->eq_back, PROTO_NAME_LEN);
    put_str(w, m->eq_hand_r, PROTO_NAME_LEN);
    put_str(w, m->eq_hand_l, PROTO_NAME_LEN);
    put_str(w, m->eq_hands, PROTO_NAME_LEN);
    put_str(w, m->eq_arm_r, PROTO_NAME_LEN);
    put_str(w, m->eq_arm_l, PROTO_NAME_LEN);
    put_str(w, m->eq_feet, PROTO_NAME_LEN);
    for (int i = 0; i < 10; i++)
        put_str(w, m->eq_ring[i], PROTO_NAME_LEN);
    for (int i = 0; i < 4; i++)
        put_str(w, m->eq_belt[i], PROTO_NAME_LEN);
    bw_bool(w, m->is_merchant != 0);
    put_i8(w, m->shop_spec);
    bw_bool(w, m->is_tombstone != 0);
    bw_bool(w, m->is_player != 0);
    put_str(w, m->username, sizeof(m->username));
    return frame_end(w, at);
}

bool wire_get_state(const void *p, size_t n, MsgState *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->gold = br_u64(&r);
    m->entity_id = br_i32(&r);
    m->x = br_i32(&r);
    m->y = br_i32(&r);
    m->hp = br_i32(&r);
    m->max_hp = br_i32(&r);
    m->floor_id = br_i32(&r);
    m->game_hour = br_i32(&r);
    m->game_min = br_i32(&r);
    m->total_turns = br_i32(&r);
    m->str = br_i32(&r);
    m->dex = br_i32(&r);
    m->con = br_i32(&r);
    m->intel = br_i32(&r);
    m->wis = br_i32(&r);
    m->cha = br_i32(&r);
    m->movement_cooldown = get_f32(&r);
    m->equipped_mask = br_i32(&r);
    m->level = br_i32(&r);
    m->xp = br_i32(&r);
    m->vision_radius = br_i32(&r);
    m->ac = br_i32(&r);
    m->status_icons = br_u32(&r);
    get_str(&r, m->weapon_name, sizeof(m->weapon_name));
    get_str(&r, m->armor_name, sizeof(m->armor_name));
    m->to_hit = br_i32(&r);
    m->to_dmg = br_i32(&r);
    m->bosses_defeated = br_u32(&r);
    m->hunger_level = br_i32(&r);
    for (int i = 0; i < 10; i++)
        m->spell_slots[i] = br_i32(&r);
    for (int i = 0; i < 10; i++)
        m->spell_slots_max[i] = br_i32(&r);
    get_str(&r, m->eq_head, PROTO_NAME_LEN);
    get_str(&r, m->eq_neck, PROTO_NAME_LEN);
    get_str(&r, m->eq_body, PROTO_NAME_LEN);
    get_str(&r, m->eq_back, PROTO_NAME_LEN);
    get_str(&r, m->eq_hand_r, PROTO_NAME_LEN);
    get_str(&r, m->eq_hand_l, PROTO_NAME_LEN);
    get_str(&r, m->eq_hands, PROTO_NAME_LEN);
    get_str(&r, m->eq_arm_r, PROTO_NAME_LEN);
    get_str(&r, m->eq_arm_l, PROTO_NAME_LEN);
    get_str(&r, m->eq_feet, PROTO_NAME_LEN);
    for (int i = 0; i < 10; i++)
        get_str(&r, m->eq_ring[i], PROTO_NAME_LEN);
    for (int i = 0; i < 4; i++)
        get_str(&r, m->eq_belt[i], PROTO_NAME_LEN);
    m->is_merchant = br_bool(&r) ? 1 : 0;
    m->shop_spec = get_i8(&r);
    m->is_tombstone = br_bool(&r) ? 1 : 0;
    m->is_player = br_bool(&r) ? 1 : 0;
    get_str(&r, m->username, sizeof(m->username));
    return br_done(&r);
}

bool wire_put_map_chunk(BinWriter *w, const MsgMapChunk *m, const uint8_t *tiles) {
    if (m->width <= 0 || m->height <= 0 || m->width > WIRE_MAX_CHUNK_SIDE ||
        m->height > WIRE_MAX_CHUNK_SIDE)
        return false;
    size_t at = frame_begin(w, MSG_MAP_CHUNK);
    put_i16(w, m->start_x);
    put_i16(w, m->start_y);
    put_i16(w, m->width);
    put_i16(w, m->height);
    bw_bytes(w, tiles, (size_t)m->width * (size_t)m->height);
    return frame_end(w, at);
}

bool wire_get_map_chunk(const void *p, size_t n, MsgMapChunk *m, const uint8_t **tiles) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    *tiles = NULL;
    m->start_x = (int16_t)get_i16(&r);
    m->start_y = (int16_t)get_i16(&r);
    m->width = (int16_t)get_i16(&r);
    m->height = (int16_t)get_i16(&r);
    if (r.failed || m->width <= 0 || m->height <= 0 || m->width > WIRE_MAX_CHUNK_SIDE ||
        m->height > WIRE_MAX_CHUNK_SIDE || m->start_x < 0 || m->start_y < 0 ||
        m->start_x + m->width > MAP_WIDTH || m->start_y + m->height > MAP_HEIGHT)
        return false;
    size_t count = (size_t)m->width * (size_t)m->height;
    const uint8_t *t = br_view(&r, count);
    if (!t || !br_done(&r))
        return false;
    for (size_t i = 0; i < count; i++)
        if (t[i] >= VOXEL_COUNT)
            return false;
    *tiles = t;
    return true;
}

bool wire_put_text(BinWriter *w, const char *text) {
    size_t at = frame_begin(w, MSG_TEXT);
    put_str(w, text, PROTO_TEXT_LEN);
    return frame_end(w, at);
}

bool wire_get_text(const void *p, size_t n, MsgText *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    get_str(&r, m->text, sizeof(m->text));
    return br_done(&r);
}

bool wire_put_auth_fail(BinWriter *w, const char *reason) {
    size_t at = frame_begin(w, MSG_AUTH_FAIL);
    put_str(w, reason, PROTO_REASON_LEN);
    return frame_end(w, at);
}

bool wire_get_auth_fail(const void *p, size_t n, MsgAuthFail *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    get_str(&r, m->reason, sizeof(m->reason));
    return br_done(&r);
}

bool wire_put_spell_vfx(BinWriter *w, const MsgSpellVFX *m) {
    size_t at = frame_begin(w, MSG_SPELL_VFX);
    put_i16(w, m->start_x);
    put_i16(w, m->start_y);
    put_i16(w, m->target_x);
    put_i16(w, m->target_y);
    bw_u8(w, (uint8_t)m->vfx_type);
    put_unit(w, m->color_r);
    put_unit(w, m->color_g);
    put_unit(w, m->color_b);
    return frame_end(w, at);
}

bool wire_get_spell_vfx(const void *p, size_t n, MsgSpellVFX *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->start_x = get_i16(&r);
    m->start_y = get_i16(&r);
    m->target_x = get_i16(&r);
    m->target_y = get_i16(&r);
    m->vfx_type = br_u8(&r);
    m->color_r = get_unit(&r);
    m->color_g = get_unit(&r);
    m->color_b = get_unit(&r);
    return br_done(&r);
}

bool wire_put_tombstone_remove(BinWriter *w, int entity_id) {
    size_t at = frame_begin(w, MSG_TOMBSTONE_REMOVE);
    bw_i32(w, entity_id);
    return frame_end(w, at);
}

bool wire_get_tombstone_remove(const void *p, size_t n, MsgTombstoneRemove *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->entity_id = br_i32(&r);
    return br_done(&r);
}

bool wire_put_time_sync(BinWriter *w, const MsgTimeSync *m) {
    size_t at = frame_begin(w, MSG_TIME_SYNC);
    bw_u8(w, (uint8_t)m->game_hour);
    bw_u8(w, (uint8_t)m->game_min);
    bw_i32(w, m->total_turns);
    return frame_end(w, at);
}

bool wire_get_time_sync(const void *p, size_t n, MsgTimeSync *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->game_hour = br_u8(&r);
    m->game_min = br_u8(&r);
    m->total_turns = br_i32(&r);
    return br_done(&r) && m->game_hour < 24 && m->game_min < 60;
}

bool wire_put_entity_update(BinWriter *w, const EntityUpdateRec *recs, int count) {
    if (count <= 0 || count > WIRE_MAX_ENTITY_RECS)
        return false;
    size_t at = frame_begin(w, MSG_ENTITY_UPDATE);
    bw_u16(w, (uint16_t)count);
    for (int i = 0; i < count; i++) {
        bw_i32(w, recs[i].entity_id);
        put_i16(w, recs[i].x);
        put_i16(w, recs[i].y);
        bw_i32(w, recs[i].hp);
    }
    return frame_end(w, at);
}

bool wire_get_entity_update(const void *p, size_t n, EntityUpdateRec *out, int cap,
                            int *count) {
    BinReader r;
    br_init(&r, p, n);
    *count = 0;
    uint32_t c = br_u16(&r);
    if (r.failed || c == 0 || c > WIRE_MAX_ENTITY_RECS || (int)c > cap ||
        n != 2 + (size_t)c * WIRE_ENTITY_REC_SIZE)
        return false;
    for (uint32_t i = 0; i < c; i++) {
        out[i].entity_id = br_i32(&r);
        out[i].x = (int16_t)get_i16(&r);
        out[i].y = (int16_t)get_i16(&r);
        out[i].hp = br_i32(&r);
    }
    if (!br_done(&r))
        return false;
    *count = (int)c;
    return true;
}

bool wire_put_combat_event(BinWriter *w, const MsgCombatEvent *m) {
    if (m->kind < 1 || m->kind > COMBAT_EV_MAX)
        return false;
    size_t at = frame_begin(w, MSG_COMBAT_EVENT);
    bw_u8(w, (uint8_t)m->kind);
    bw_i32(w, m->entity_id);
    bw_i32(w, m->amount);
    return frame_end(w, at);
}

bool wire_get_combat_event(const void *p, size_t n, MsgCombatEvent *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->kind = br_u8(&r);
    m->entity_id = br_i32(&r);
    m->amount = br_i32(&r);
    return br_done(&r) && m->kind >= 1 && m->kind <= COMBAT_EV_MAX;
}
