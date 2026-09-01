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

#include <stdint.h>
#include "map.h"

typedef uint32_t MsgType;
#define MSG_LOGIN      1
#define MSG_WELCOME    2
#define MSG_MOVE       3
#define MSG_STATE      4
#define MSG_MAP_CHUNK  5
#define MSG_TEXT_CMD   6
#define MSG_TEXT       7
#define MSG_AUTH_FAIL  8
#define MSG_SPELL_VFX  9
#define MSG_TOMBSTONE_REMOVE 10
#define MSG_TIME_SYNC 11

typedef struct {
    MsgType type;
    int length;
} MsgHeader;

/*Sent by the client at login.
   If is_new_char == 1: The client wants to create a new character.
   race_id and class_id are used for creation only.*/
typedef struct {
    char username[32];
    char password[32];
    char server_pass[32];
    int  race_id;       /* RaceType  */
    int  subrace_id;    /* SubraceType */
    int  class_id;      /* ClassType */
    int  is_new_char;   /*1 = request for new character*/
    int  str, dex, con, intel, wis, cha;
    int  age, height, weight, social_class, alignment;
} MsgLogin;

/*Sent by the server after successful authentication.*/
typedef struct {
    int entity_id;
    int x;
    int y;
    int success;    /* 1 = OK, 0 = fallito */
    int is_new;     /*1 = new character created*/
    int hp;
    int max_hp;
    uint64_t gold;
    int race_id;
    int subrace_id;
    int class_id;
    int level;
    int alignment;
} MsgWelcome;

typedef struct {
    int entity_id;
    int dx;
    int dy;
    int dz;
} MsgMove;

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
    uint32_t status_icons; //Bitmask for status icons
    char weapon_name[32];
    char armor_name[32];
    int to_hit;
    int to_dmg;
    uint32_t bosses_defeated; // bitmask for 10 bosses
    int hunger_level;
    int spell_slots[10];
    int spell_slots_max[10];
    char eq_head[32];
    char eq_neck[32];
    char eq_body[32];
    char eq_back[32];
    char eq_hand_r[32];
    char eq_hand_l[32];
    char eq_hands[32];
    char eq_arm_r[32];
    char eq_arm_l[32];
    char eq_feet[32];
    char eq_ring[10][32];
    char eq_belt[4][32];
    int  is_merchant;   /*1 = entity is a merchant*/
    int  shop_spec;    /*MerchantSpecialization of the shop (SHOP_SPEC_NONE if not a merchant)*/
    int  is_tombstone;  /*1 = entity is a tombstone*/
    int  is_player;
    char username[32];
} MsgState;

/* Values for MsgState.shop_spec - they must match the
 * MerchantSpecialization enum in src/server/server_entities.h.
 * The client only needs to recognize the martial bookshop to
 * draw its unique sprite.*/
#define SHOP_SPEC_NONE          (-1)
#define SHOP_SPEC_BOOKS_MARTIAL 11

/*Sent by the server when a tombstone is removed (item recovery)*/
typedef struct {
    int entity_id; /*ID of the tombstone to remove*/
} MsgTombstoneRemove;

typedef struct {
    int start_x;
    int start_y;
    int width;
    int height;
} MsgMapChunk;

typedef struct {
    char cmd[256];
} MsgTextCmd;

typedef struct {
    char text[256];
} MsgText;

/*Sent by the server when authentication fails.
   The reason field explains the reason.*/
typedef struct {
    char reason[128];
} MsgAuthFail;


typedef struct {
    int start_x;
    int start_y;
    int target_x;
    int target_y;
    int vfx_type;
    float color_r;
    float color_g;
    float color_b;
} MsgSpellVFX;

typedef struct {
    int game_hour;
    int game_min;
    int total_turns;
} MsgTimeSync;

#endif // PROTOCOL_H

