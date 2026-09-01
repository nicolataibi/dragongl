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

#ifndef SERVER_ENTITIES_H
#define SERVER_ENTITIES_H

#include <stdint.h>
#include <stdbool.h>
#include "rules.h"
#include "items.h"
#include "bestiary.h"
#include "net.h"
#include "species.h"
#include "classes.h"
#include "spells.h"

/* Size in uint64_t of the bitfield for known spells.
 * MAX_SPELL_DB_SIZE is defined in spells.h (512).
 * 512 / 64 = 8 words of 64 bits = 64 bytes. */
#define KNOWN_SPELLS_WORDS (MAX_SPELL_DB_SIZE / 64)

#define MAX_CLIENTS 64
#define MAX_NPCS 50000
#define MAX_EFFECTS_PER_ENTITY 16
#define MAX_BACKPACK 32
#define MAX_BELT 4
#define MAX_SPELL_LEVEL 9
#define MAX_SHOP_ITEMS 50
#define MAX_BLOCKED_PLAYERS 16

#define HUNGER_MAX 2000
#define HUNGER_ALERT 1200
#define HUNGER_WEAK 1600
#define HUNGER_FAINT 1850

typedef enum {
    ARCH_MELEE,
    ARCH_CASTER,
    ARCH_ASSASSIN,
    ARCH_BRUTE,
    ARCH_DRAGON,
    ARCH_BOSS,
    ARCH_MERCHANT,
    ARCH_TREASURE,
    ARCH_GOLD,
    ARCH_SWARM,
    ARCH_COUNT
} EntityArchetype;

typedef enum {
    SHOP_GENERAL,
    BLACKSMITH,
    ALCHEMIST,
    MAGIC_SHOP,
    PROVISIONER,
    BLACK_MARKET,
    SCROLLS,
    WANDS_STAFFS,
    JEWELRY,
    BOOKS_MAGE,
    BOOKS_PRIEST,
    BOOKS_MARTIAL,
    CURRENCY_EXCHANGE,
    SPEC_COUNT
} MerchantSpecialization;

typedef struct {
    int item_templates[MAX_SHOP_ITEMS];
    int item_stock[MAX_SHOP_ITEMS];
    int item_stock_max[MAX_SHOP_ITEMS];
    int item_count;
    int restock_timer;
    char shop_name[64];
    MerchantSpecialization spec;
} MerchantData;

// AI Node status
typedef enum {
    AI_SUCCESS,
    AI_FAILURE,
    AI_RUNNING
} AINodeStatus;

// Context for NPC Artificial Intelligence
typedef struct {
    int current_target_id;
    int state_timer;
    int legendary_actions_spent;
    void* behavior_tree_root; // Points to an AINodeFunc
} AIContext;

typedef struct {
    int entity_id, x, y, floor_id, hp, max_hp;
    bool active;
    EntityArchetype archetype;
    int template_idx; //Index in bestiary_data (if applicable)
    const MonsterTemplate *template;
    
    // Dynamic Stats
    int ac, attack_bonus, damage_dice, damage_sides, xp_reward, gold_drop, morale;
    
    // Systems
    ActiveEffect effects[MAX_EFFECTS_PER_ENTITY];
    int effect_count;
    int spell_slots[MAX_SPELL_LEVEL + 1];
    int spell_slots_max[MAX_SPELL_LEVEL + 1];
    AIContext ai_ctx;
    
    MerchantData merchant;
    
    // Bones / Ghost system
    bool is_ghost;
    char custom_name[64];
    ItemInstance ghost_loot[30];
    
    //Respawn
    int spawn_x, spawn_y, respawn_timer;

    // Speed/Tick System (Phase 4)
    //The entity acts when energy >= ENERGY_THRESHOLD.
    //Each tick receives energy equal to (template->speed or 2 if not defined).
    //speed=1: slow (acts every 2 ticks), speed=2: normal, speed=3: fast (acts 2x per tick)
    int energy;
} EntityInstance;

typedef EntityInstance NPC; // Temporary alias for backward compatibility

typedef struct {
    int sock, entity_id, x, y, floor_id;
    bool active, authenticated, is_dm;
    char username[32], password[32];
    RaceType race_id;
    SubraceType subrace_id;
    ClassType class_id;
    int level, xp, str, dex, con, intel, wis, cha;
    uint64_t gold; int hp, max_hp;
    ItemInstance backpack[MAX_BACKPACK]; int backpack_count;
    ItemInstance belt[MAX_BELT]; 
    ItemInstance slot_head, slot_neck, slot_body, slot_back, slot_hand_r, slot_hand_l, slot_hands, slot_arm_r, slot_arm_l, slot_feet;
    ItemInstance slot_rings[10];
    int light_turns_left, hunger_level, exhaustion_level;
    int spell_slots[MAX_SPELL_LEVEL + 1], spell_slots_max[MAX_SPELL_LEVEL + 1];
    ActiveEffect effects[MAX_EFFECTS_PER_ENTITY];
    int effect_count;
    // Bargaining system
    int pending_trade_item_idx;      //Item index (store or backpack)
    uint64_t pending_trade_price;    //Negotiated price
    int pending_trade_merchant_id;   //ID of the merchant you trade with (-1 if none)
    bool pending_trade_is_buy;       // true = purchase, false = sale
    int pending_trade_attempts;      //Number of consecutive attempts
    int alignment;                   // 0-8: Lawful Good -> Chaotic Evil
    
    /* --- MMO System --- */
    char party_leader[32];           // Empty string means not in a party. Otherwise holds the party leader's username.
    char pending_invite[32];         //Username of the person who invited you to the party.
    char trading_with[32];           //Username of the player you are trading with
    bool trade_accepted;             //true if he accepted the trade
    int trade_offer_item_idx;        //Backpack index of the offered item (-1 = none)
    uint64_t trade_offer_gold;       //Gold offered

    /*--- Messaging: block list and anti-spam ---*/
    char blocked_players[MAX_BLOCKED_PLAYERS][32]; //Username of blocked players
    int blocked_count;                             //Number of entries in the block list
    long long last_msg_ms;                         //Timestamp of the last msg/say (anti-spam)

    /* --- Phase 3: Persistent statistics --- */
    int total_kills;                 //Total number of NPCs eliminated
    int total_steps;                 //Total number of steps taken
    /* --- Innate transit magic: deepest floor ever reached by this character.
     * Used by the 12 class transit cantrips (surface <-> deepest explored). */
    int max_floor_explored;
    /* --- Phase 4: Advanced Speed/Tick System --- */
    long long last_action_ms;        //Timestamp (ms) for throttling (anti-spam)
    /*--- Phase 5: Grimoire - spells learned ---
* Compact bitfield: bit N = 1 → magic index N in spell_database known.
     * Test: (known_spells[N/64] >> (N%64)) & 1
     * Set: known_spells[N/64] |= (1ULL << (N%64))
     * Clear: known_spells[N/64] &= ~(1ULL << (N%64))*/
    uint64_t known_spells[KNOWN_SPELLS_WORDS];
    /*--- Stage 6: Boss Flags ---*/
    uint32_t bosses_defeated;        // Bitmask for defeated bosses
    bool needs_study;                // Indicator for level up study HUD
    int unspent_stat_points;         // Stat points to distribute
} Client;


typedef struct {
    char password[32]; int x, y, floor_id;
    RaceType race_id;
    SubraceType subrace_id;
    ClassType class_id;
    int level, xp, str, dex, con, intel, wis, cha;
    uint64_t gold; int hp, max_hp;
    ItemInstance backpack[MAX_BACKPACK]; int backpack_count;
    ItemInstance belt[MAX_BELT];
    ItemInstance s_head, s_neck, s_body, s_back, s_hand_r, s_hand_l, s_hands, s_arm_r, s_arm_l, s_feet, s_rings[10];
    int spell_slots[MAX_SPELL_LEVEL + 1], spell_slots_max[MAX_SPELL_LEVEL + 1];
    int alignment;
    /*--- Step 3: Fields Added for Save State Complete ---*/
    ActiveEffect effects[MAX_EFFECTS_PER_ENTITY]; /*Active status effects*/
    int effect_count;                             /*Number of saved effects*/
    int light_turns_left;                         /*Torch/Magic light remaining*/
    int hunger_level;                             /*Current hunger level*/
    int exhaustion_level;                         /*Exhaustion level*/
    int total_kills;                              /* Stats: total kills        */
    int total_steps;                              /* Stats: total steps        */
    int max_floor_explored;                       /*Deepest plane reached (innate transit)*/
    /* --- Phase 5: Persistent grimoire --- */
    uint64_t known_spells[KNOWN_SPELLS_WORDS];    /* Bitfield of known spells  */
    /*--- Stage 6: Boss Flags ---*/
    uint32_t bosses_defeated;                     /* Bitmask for defeated bosses */
    int unspent_stat_points;
    /*--- Messaging: Persistent block list ---*/
    char blocked_players[MAX_BLOCKED_PLAYERS][32]; /* Blocked players */
    int blocked_count;                             /*Number of entries in the list*/
} SaveData;


void perform_attack_npc(NPC *n, Client *c, NPC *all_npcs);
void send_text_to_client(int sock, const char* fmt, ...);

#endif
