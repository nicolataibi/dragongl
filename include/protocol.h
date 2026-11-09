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

#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * Wire protocol, version 4.
 *
 * The structs below are the IN-MEMORY form of each message. None of them
 * is ever sent as raw bytes: src/shared/wire.c encodes and decodes every
 * message field by field, little-endian, with explicit widths, so the
 * format depends neither on the compiler's struct layout nor on the host
 * byte order. See PROTOCOL.md for the byte-level description.
 *
 * Frame:  u16 version | u16 type | u32 payload length | payload
 *
 * Strings travel as u8 length + bytes (no NUL); a length that does not
 * fit the receiving field is a protocol error. A payload must be consumed
 * exactly: trailing bytes are an error too.
 */

#include <stdint.h>
#include "map.h"

typedef uint16_t MsgType;
#define MSG_LOGIN            1  /* C -> S */
#define MSG_WELCOME          2  /* S -> C */
#define MSG_MOVE             3  /* C -> S */
#define MSG_STATE            4  /* S -> C */
#define MSG_MAP_CHUNK        5  /* S -> C */
#define MSG_TEXT_CMD         6  /* C -> S */
#define MSG_TEXT             7  /* S -> C */
#define MSG_AUTH_FAIL        8  /* S -> C */
#define MSG_SPELL_VFX        9  /* S -> C */
#define MSG_TOMBSTONE_REMOVE 10 /* S -> C */
#define MSG_TIME_SYNC        11 /* S -> C */
#define MSG_ENTITY_UPDATE    12 /* S -> C */
#define MSG_COMBAT_EVENT     13 /* S -> C */

/*Both ends reject any other value: an old client and a new server fail
 * at the first message with a clear error instead of misreading it.
 * v4: explicit serialization (wire.c), 8-byte header, length-prefixed
 * strings, 64-byte credential fields, MSG_COMBAT_EVENT.*/
#define PROTOCOL_VERSION 4

#define WIRE_HEADER_SIZE 8
/*Largest payload either side accepts (a map chunk is the biggest
 * message: 8 + 256*256 tile bytes).*/
#define WIRE_MAX_PAYLOAD 73728u /*72 KiB*/
/*Largest payload a client may send (MSG_LOGIN): the server's receive
 * buffer is sized from it.*/
#define WIRE_MAX_CLIENT_PAYLOAD 256u

/*Decoded frame header.*/
typedef struct {
    uint16_t version;
    MsgType type;
    uint32_t length; /*payload bytes after the header*/
} MsgHeader;

/*Text field sizes, NUL included (the wire carries at most size-1 bytes).*/
#define PROTO_NAME_LEN   32  /* username, equipment and item names */
#define PROTO_SECRET_LEN 64  /* personal and server passwords */
#define PROTO_TEXT_LEN   256 /* MSG_TEXT, MSG_TEXT_CMD */
#define PROTO_REASON_LEN 128 /* MSG_AUTH_FAIL */

/*MSG_LOGIN. Creation fields (race..alignment) are used only when the
 * character does not exist yet; the ability scores are rolled by the
 * server.*/
typedef struct {
    char username[PROTO_NAME_LEN];
    char password[PROTO_SECRET_LEN];
    char server_pass[PROTO_SECRET_LEN];
    int race_id;    /* RaceType, -> i8 */
    int subrace_id; /* SubraceType or -1, -> i8 */
    int class_id;   /* ClassType, -> i8 */
    int alignment;  /* -> i8 */
} MsgLogin;

/*MSG_WELCOME: the login succeeded.*/
typedef struct {
    int entity_id;
    int x, y;
    int is_new; /*1 = the character was just created*/
    int hp, max_hp;
    uint64_t gold;
    int race_id, subrace_id, class_id;
    int level;
    int alignment;
} MsgWelcome;

/*MSG_MOVE: one step, dx and dy in -1..1 (-> i8 each).*/
typedef struct {
    int dx;
    int dy;
} MsgMove;

/*MSG_STATE: the full sheet of the player's own character, or the
 * identity of another entity the client has not seen yet.*/
typedef struct {
    uint64_t gold;
    int entity_id;
    int x, y, hp, max_hp;
    int floor_id;
    int game_hour, game_min, total_turns;
    int str, dex, con, intel, wis, cha;
    float movement_cooldown;
    int equipped_mask;
    int level;
    int xp;
    int vision_radius;
    int ac;
    uint32_t status_icons; /*bitmask of status icons*/
    char weapon_name[PROTO_NAME_LEN];
    char armor_name[PROTO_NAME_LEN];
    int to_hit;
    int to_dmg;
    uint32_t bosses_defeated; /*bitmask for 10 bosses*/
    int hunger_level;
    int spell_slots[10];
    int spell_slots_max[10];
    char eq_head[PROTO_NAME_LEN];
    char eq_neck[PROTO_NAME_LEN];
    char eq_body[PROTO_NAME_LEN];
    char eq_back[PROTO_NAME_LEN];
    char eq_hand_r[PROTO_NAME_LEN];
    char eq_hand_l[PROTO_NAME_LEN];
    char eq_hands[PROTO_NAME_LEN];
    char eq_arm_r[PROTO_NAME_LEN];
    char eq_arm_l[PROTO_NAME_LEN];
    char eq_feet[PROTO_NAME_LEN];
    char eq_ring[10][PROTO_NAME_LEN];
    char eq_belt[4][PROTO_NAME_LEN];
    int is_merchant;  /*1 = entity is a merchant*/
    int shop_spec;    /*MerchantSpecialization (SHOP_SPEC_NONE if not a merchant)*/
    int is_tombstone; /*1 = entity is a tombstone*/
    int is_player;
    char username[PROTO_NAME_LEN];
} MsgState;

/* Values for MsgState.shop_spec - they must match the
 * MerchantSpecialization enum in src/server/server_entities.h.
 * The client only needs to recognize the martial bookshop to
 * draw its unique sprite.*/
#define SHOP_SPEC_NONE          (-1)
#define SHOP_SPEC_BOOKS_MARTIAL 11

/*MSG_ENTITY_UPDATE carries u16 count + count records: position and HP of
 * entities the client already received a full MSG_STATE for.*/
typedef struct {
    int32_t entity_id;
    int16_t x;
    int16_t y;
    int32_t hp;
} EntityUpdateRec;
#define WIRE_ENTITY_REC_SIZE 12
#define WIRE_MAX_ENTITY_RECS 4096

/*MSG_TOMBSTONE_REMOVE: a tombstone was emptied.*/
typedef struct {
    int entity_id;
} MsgTombstoneRemove;

/*MSG_MAP_CHUNK: a rectangle of the floor, followed by width*height tile
 * bytes (row by row) that stay in the payload buffer.*/
typedef struct {
    int16_t start_x;
    int16_t start_y;
    int16_t width;
    int16_t height;
} MsgMapChunk;
#define WIRE_MAX_CHUNK_SIDE 256

typedef struct {
    char cmd[PROTO_TEXT_LEN];
} MsgTextCmd;

typedef struct {
    char text[PROTO_TEXT_LEN];
} MsgText;

/*MSG_AUTH_FAIL: why the login (or the connection) was refused.*/
typedef struct {
    char reason[PROTO_REASON_LEN];
} MsgAuthFail;

/*MSG_SPELL_VFX: a spell effect to draw. The colour travels as 8-bit
 * channels.*/
typedef struct {
    int start_x, start_y;
    int target_x, target_y;
    int vfx_type;
    float color_r, color_g, color_b; /* 0..1 */
} MsgSpellVFX;

typedef struct {
    int game_hour;
    int game_min;
    int total_turns;
} MsgTimeSync;

/*MSG_COMBAT_EVENT: what the floating combat text shows, said by the
 * server (never guessed from the wording of MSG_TEXT lines). entity_id is
 * the entity the number belongs to (the player, or the monster that was
 * hit), 0 if none.*/
typedef enum {
    COMBAT_EV_DAMAGE_TAKEN = 1, /* the player lost `amount` HP */
    COMBAT_EV_DAMAGE_DEALT = 2, /* the player hit a monster for `amount` */
    COMBAT_EV_CRITICAL = 3,     /* ... with a critical hit */
    COMBAT_EV_MISS_TAKEN = 4,   /* a monster missed the player */
    COMBAT_EV_MISS_DEALT = 5,   /* the player missed */
    COMBAT_EV_HEAL = 6,         /* the player recovered `amount` HP */
    COMBAT_EV_GOLD = 7,         /* the player picked up `amount` gold */
    COMBAT_EV_LEVEL_UP = 8,     /* the player reached level `amount` */
    COMBAT_EV_MAX = 8
} CombatEventKind;

typedef struct {
    int kind; /* CombatEventKind, -> u8 */
    int entity_id;
    int amount;
} MsgCombatEvent;

#endif // PROTOCOL_H
