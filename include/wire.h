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

/*
 * Encoding and decoding of the wire protocol (protocol.h, PROTOCOL.md).
 *
 * Encoders append ONE complete frame (header + payload) to a BinWriter
 * and return false only if the writer ran out of memory or the payload
 * would exceed WIRE_MAX_PAYLOAD. Several frames can be appended to the
 * same writer and sent with a single write.
 *
 * Decoders take a payload (the bytes after the header) and fill the
 * struct; they return false for any malformed payload: truncated, with
 * trailing bytes, a string longer than its field, a count or enum out of
 * range. A false return is a protocol violation: the peer is dropped.
 */

#ifndef DRAGONGL_WIRE_H
#define DRAGONGL_WIRE_H

#include "binio.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Frame header ---------------------------------------------------------- */
/* Decode the 8 header bytes at buf (no validation). */
void wire_read_header(const uint8_t *buf, MsgHeader *h);
/* Largest payload the server accepts for a message a client may send,
 * or 0 if a client may not send that type. */
uint32_t wire_client_payload_max(MsgType type);
/* Largest payload the client accepts for a message the server may send,
 * or 0 if the server may not send that type. */
uint32_t wire_server_payload_max(MsgType type);

/* --- Client -> server ------------------------------------------------------ */
bool wire_put_login(BinWriter *w, const MsgLogin *m);
bool wire_get_login(const void *p, size_t n, MsgLogin *m);
bool wire_put_move(BinWriter *w, const MsgMove *m);
bool wire_get_move(const void *p, size_t n, MsgMove *m);
bool wire_put_text_cmd(BinWriter *w, const char *cmd);
bool wire_get_text_cmd(const void *p, size_t n, MsgTextCmd *m);

/* --- Server -> client ------------------------------------------------------ */
bool wire_put_welcome(BinWriter *w, const MsgWelcome *m);
bool wire_get_welcome(const void *p, size_t n, MsgWelcome *m);
bool wire_put_state(BinWriter *w, const MsgState *m);
bool wire_get_state(const void *p, size_t n, MsgState *m);
/* tiles: width*height bytes, row by row. */
bool wire_put_map_chunk(BinWriter *w, const MsgMapChunk *m, const uint8_t *tiles);
/* *tiles points into p (width*height bytes, every value < VOXEL_COUNT). */
bool wire_get_map_chunk(const void *p, size_t n, MsgMapChunk *m, const uint8_t **tiles);
bool wire_put_text(BinWriter *w, const char *text);
bool wire_get_text(const void *p, size_t n, MsgText *m);
bool wire_put_auth_fail(BinWriter *w, const char *reason);
bool wire_get_auth_fail(const void *p, size_t n, MsgAuthFail *m);
bool wire_put_spell_vfx(BinWriter *w, const MsgSpellVFX *m);
bool wire_get_spell_vfx(const void *p, size_t n, MsgSpellVFX *m);
bool wire_put_tombstone_remove(BinWriter *w, int entity_id);
bool wire_get_tombstone_remove(const void *p, size_t n, MsgTombstoneRemove *m);
bool wire_put_time_sync(BinWriter *w, const MsgTimeSync *m);
bool wire_get_time_sync(const void *p, size_t n, MsgTimeSync *m);
/* count in 1..WIRE_MAX_ENTITY_RECS. */
bool wire_put_entity_update(BinWriter *w, const EntityUpdateRec *recs, int count);
/* Up to cap records into out; *count receives how many. */
bool wire_get_entity_update(const void *p, size_t n, EntityUpdateRec *out, int cap, int *count);
bool wire_put_combat_event(BinWriter *w, const MsgCombatEvent *m);
bool wire_get_combat_event(const void *p, size_t n, MsgCombatEvent *m);

#endif
