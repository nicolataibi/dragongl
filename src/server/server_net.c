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
#include "server_net.h"
#include "server_internal.h"
#include "server_conn.h"
#include "protocol.h"
#include "wire.h"
#include "map.h"
#include "game.h"
#include "player.h"
#include "server_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/*Every frame is encoded into this writer (the game loop is the only
 * sender) and queued with a single net_send_client(); its memory is
 * reused from one message to the next. A broadcast encodes once and
 * queues the same bytes for every receiver.*/
static BinWriter g_tx;

static BinWriter *tx_frame(void) {
    bw_reset(&g_tx);
    return &g_tx;
}

static bool tx_send(int sock) {
    return !g_tx.failed && g_tx.len > 0 && net_send_client(sock, g_tx.data, (int)g_tx.len);
}

void send_text_to_client(int sock, const char *fmt, ...) {
    MsgText t;
    va_list a;
    va_start(a, fmt);
    vsnprintf(t.text, sizeof(t.text), fmt, a);
    va_end(a);
    if (wire_put_text(tx_frame(), &t)) {
        tx_send(sock);
    }
}

void send_combat_event(int sock, int kind, int entity_id, int amount) {
    MsgCombatEvent ev = {kind, entity_id, amount};
    if (wire_put_combat_event(tx_frame(), &ev)) {
        tx_send(sock);
    }
}

bool send_welcome(int sock, const MsgWelcome *m) {
    return wire_put_welcome(tx_frame(), m) && tx_send(sock);
}

void send_time_sync(int sock, const MsgTimeSync *m) {
    if (wire_put_time_sync(tx_frame(), m)) {
        tx_send(sock);
    }
}

void send_tombstone_remove(int sock, int entity_id) {
    MsgTombstoneRemove m = {entity_id};
    if (wire_put_tombstone_remove(tx_frame(), &m)) {
        tx_send(sock);
    }
}

/*Queue a message for a client. Sends never block and never disconnect
 * synchronously: a client whose queue overflows, or whose socket is dead,
 * is marked and reaped by the main loop (save, "left floor" notice,
 * close) — see server_conn.h. Returns false if the client is being
 * dropped; later sends to it in the same tick are discarded.*/
bool net_send_client(int sock, const void *data, int len) {
    if (len < 0) {
        return false;
    }
    int slot = conn_slot_for_sock(sock);
    if (slot < 0) {
        return false;
    }
    return conn_queue(slot, data, (size_t)len);
}

void send_auth_fail(int sock, const char *reason) {
    MsgAuthFail m;
    copy_str(m.reason, reason, sizeof(m.reason));
    if (wire_put_auth_fail(tx_frame(), &m)) {
        tx_send(sock);
    }
}

void send_map_chunk(int sock, Map *map, int cx, int cy, int size) {
    MsgMapChunk mc;
    if (size <= 0 || size > WIRE_MAX_CHUNK_SIDE || size > MAP_WIDTH) { /*the map is square*/
        return;
    }
    int sx = cx - size / 2, sy = cy - size / 2;
    if (sx < 0) {
        sx = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    if (sx + size > MAP_WIDTH) {
        sx = MAP_WIDTH - size;
    }
    if (sy + size > MAP_HEIGHT) {
        sy = MAP_HEIGHT - size;
    }
    mc.start_x = sx;
    mc.start_y = sy;
    mc.width = size;
    mc.height = size;
    static uint8_t buf[WIRE_MAX_CHUNK_SIDE * WIRE_MAX_CHUNK_SIDE];
    int idx = 0;
    for (int iy = sy; iy < sy + size; iy++) {
        for (int ix = sx; ix < sx + size; ix++) {
            buf[idx++] = (uint8_t)map->data[0][iy][ix];
        }
    }
    mc.tiles = buf;
    if (wire_put_map_chunk(tx_frame(), &mc)) {
        tx_send(sock);
    }
}

/*The bit of each condition in MsgState.status_icons (bit 10: exhausted,
 * bit 11: a book waits to be studied).*/
static uint32_t state_status_icons(Client *c) {
    static const ConditionType k_icons[] = {
        COND_POISONED, COND_BLINDED,  COND_PARALYZED, COND_STUNNED, COND_UNCONSCIOUS,
        COND_BURNING,  COND_BLEEDING, COND_PETRIFIED, COND_CURSED,  COND_FROZEN};
    uint32_t icons = 0;
    for (unsigned bit = 0; bit < sizeof(k_icons) / sizeof(k_icons[0]); bit++) {
        if (rules_has_condition_t(c->effects, c->effect_count, k_icons[bit])) {
            icons |= 1u << bit;
        }
    }
    if (c->exhaustion_level > 0) {
        icons |= 1u << 10;
    }
    if (c->needs_study) {
        icons |= 1u << 11;
    }
    return icons;
}

/*The name the HUD shows for an equipment slot ("" when empty).*/
static void slot_name(const ItemInstance *slot, char dest[32]) {
    dest[0] = '\0';
    if (slot->template_idx < 0 || slot->template_idx >= item_database_size) {
        return;
    }
    copy_str(dest, slot->is_artifact ? slot->artifact_name : item_database[slot->template_idx].name,
             32);
}

static void state_equipment(Client *c, MsgState *s) {
    slot_name(&c->slot_head, s->eq_head);
    slot_name(&c->slot_neck, s->eq_neck);
    slot_name(&c->slot_body, s->eq_body);
    slot_name(&c->slot_back, s->eq_back);
    slot_name(&c->slot_hand_r, s->eq_hand_r);
    slot_name(&c->slot_hand_l, s->eq_hand_l);
    slot_name(&c->slot_hands, s->eq_hands);
    slot_name(&c->slot_arm_r, s->eq_arm_r);
    slot_name(&c->slot_arm_l, s->eq_arm_l);
    slot_name(&c->slot_feet, s->eq_feet);
    for (int i = 0; i < 10; i++) {
        slot_name(&c->slot_rings[i], s->eq_ring[i]);
    }
    for (int i = 0; i < 4; i++) {
        slot_name(&c->belt[i], s->eq_belt[i]);
    }
}

/*Weapon and armor names and the attack and damage bonuses of a melee
 * attack (the formula of perform_attack).*/
static void state_combat(Client *c, MsgState *s) {
    const ItemInstance *weapon =
        (c->slot_hand_r.template_idx != -1) ? &c->slot_hand_r : &c->slot_hand_l;
    const ItemTemplate *w =
        (weapon->template_idx != -1) ? &item_database[weapon->template_idx] : NULL;
    copy_str(s->weapon_name, w ? w->name : "Bare Hands", sizeof(s->weapon_name));
    copy_str(s->armor_name,
             c->slot_body.template_idx != -1 ? item_database[c->slot_body.template_idx].name
                                             : "None",
             sizeof(s->armor_name));
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    int a_mod = (w && w->damage_dice_sides <= 6) ? rules_get_modifier(td) : rules_get_modifier(ts);
    s->to_hit = (w ? w->attack_bonus + weapon->to_hit_bonus : 0) + a_mod + (c->level / 2) +
                rules_event_bonus(c->effects, c->effect_count, EVENT_ON_ATTACK);
    s->to_dmg = rules_get_modifier(ts) + (w ? weapon->to_dam_bonus : 0) +
                rules_event_bonus(c->effects, c->effect_count, EVENT_ON_DAMAGE);
}

void send_detailed_state(Game *game, Client *c) {
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id = c->entity_id;
    s.x = c->x;
    s.y = c->y;
    copy_str(s.username, c->username, sizeof(s.username));
    s.hp = c->hp;
    s.max_hp = c->max_hp;
    s.floor_id = c->floor_id;
    get_game_time(game, &s.game_hour, &s.game_min);
    s.total_turns = game->total_turns;
    s.str = c->str;
    s.dex = c->dex;
    s.con = c->con;
    s.intel = c->intel;
    s.wis = c->wis;
    s.cha = c->cha;
    s.movement_cooldown = get_movement_cooldown(c);
    s.equipped_mask = get_equipped_mask(c);
    s.level = c->level;
    s.xp = c->xp;
    s.gold = c->gold;
    s.vision_radius = get_vision_radius(c);
    s.ac = get_player_ac(c);
    state_combat(c, &s);
    s.status_icons = state_status_icons(c);
    s.bosses_defeated = c->bosses_defeated;
    s.hunger_level = c->hunger_level;
    for (int i = 0; i < 10; i++) {
        s.spell_slots[i] = c->spell_slots[i];
        s.spell_slots_max[i] = c->spell_slots_max[i];
    }
    state_equipment(c, &s);
    if (wire_put_state(tx_frame(), &s)) {
        tx_send(c->sock);
    }
}

void broadcast_spell_vfx(Game *game, int sx, int sy, int tx, int ty, int vfx_type, float r, float g,
                         float b, int floor_id) {
    MsgSpellVFX msg;
    msg.start_x = sx;
    msg.start_y = sy;
    msg.target_x = tx;
    msg.target_y = ty;
    msg.vfx_type = vfx_type;
    msg.color_r = r;
    msg.color_g = g;
    msg.color_b = b;
    if (!wire_put_spell_vfx(tx_frame(), &msg)) {
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (game->clients[i].active && game->clients[i].authenticated &&
            game->clients[i].floor_id == floor_id) {
            tx_send(game->clients[i].sock);
        }
    }
}

/*--- Per-client "full state already sent" bitmap ------------------------
 * The full MsgState (~1.5 KB) is sent ONCE per entity per client (the
 * first time it enters view); every following broadcast reuses a 12-byte
 * EntityUpdateRec carrying only id/x/y/hp. 64 clients * 50k bits = 400 KB.
 * g_client_seen_floor encodes the floor the bitmap is valid for (floor+1,
 * 0 = empty): it is reset lazily whenever the client is seen on another
 * floor, which is self-healing no matter which code path (stairs, trap,
 * death respawn, dm_goto) changed the floor.*/
#define BATCH_ENTITY_CAP 512
static uint8_t g_client_seen_entities[MAX_CLIENTS][MAX_NPCS / 8];
static int g_client_seen_floor[MAX_CLIENTS]; /*floor+1, 0 = empty*/

/*--- Per-client "seen PLAYERS" bitmap --------------------------------
 * Players ride the same compact path as NPCs: a full MsgState (~1 KB) the
 * first time a client sees a given player, then 12-byte EntityUpdateRec
 * records with x/y/hp on every step.
 * Player IDs are positive and unique exactly like NPC IDs (the same
 * next_id counter), and the client's MSG_ENTITY_UPDATE handler updates
 * any known id, so no protocol change is needed.
 * Row = receiver's slot, bit = sender's slot. The bitmap is reset with
 * the NPC one whenever the receiver changes floor, and the bit is
 * cleared whenever the receiver is told the player LEFT (hp<=0
 * removal): otherwise a compact update could target an entity the
 * client no longer holds in its cache (unknown ids are ignored) and
 * the player would stay invisible until the next floor change.*/
static uint8_t g_client_seen_players[MAX_CLIENTS][MAX_CLIENTS / 8];

void client_seen_reset(int ci, int floor) {
    memset(g_client_seen_entities[ci], 0, sizeof(g_client_seen_entities[ci]));
    memset(g_client_seen_players[ci], 0, sizeof(g_client_seen_players[ci]));
    g_client_seen_floor[ci] = floor + 1;
}
static inline void player_seen_set(int ci, int pi) {
    g_client_seen_players[ci][pi >> 3] |= (uint8_t)(1u << (pi & 7));
}
static inline int player_seen_test(int ci, int pi) {
    return (g_client_seen_players[ci][pi >> 3] & (1u << (pi & 7))) != 0;
}
static inline void player_seen_clear(int ci, int pi) {
    g_client_seen_players[ci][pi >> 3] &= (uint8_t) ~(1u << (pi & 7));
}
static inline void client_seen_maybe_reset(int ci, int floor) {
    if (g_client_seen_floor[ci] != floor + 1) {
        client_seen_reset(ci, floor);
    }
}
static inline void client_seen_set(int ci, int npc_i) {
    g_client_seen_entities[ci][npc_i >> 3] |= (uint8_t)(1u << (npc_i & 7));
}
static inline int client_seen_test(int ci, int npc_i) {
    return (g_client_seen_entities[ci][npc_i >> 3] & (1u << (npc_i & 7))) != 0;
}

/*Full MsgState for an NPC (used the first time the client sees it, when
 * it still needs the merchant/tombstone/identity fields).*/
static void send_full_npc_state(int sock, const NPC *n) {
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id = n->entity_id;
    s.x = n->x;
    s.y = n->y;
    s.hp = n->hp;
    s.max_hp = n->max_hp;
    s.floor_id = n->floor_id;
    s.is_merchant = (n->archetype == ARCH_MERCHANT) ? 1 : 0;
    s.shop_spec = (n->archetype == ARCH_MERCHANT) ? (int)n->merchant.spec : SHOP_SPEC_NONE;
    s.is_tombstone = 0;
    s.is_player = 0;
    if (wire_put_state(tx_frame(), &s)) {
        tx_send(sock);
    }
}

/*Full MsgState for a PLAYER (identity: username, is_player, hp/max_hp).
 * Sent the first time a client sees that player; afterwards the client
 * receives 12-byte EntityUpdateRec records (see broadcast_player_state
 * and the player section of broadcast_nearby_entities).*/
static void send_full_player_state(int sock, const Client *p) {
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id = p->entity_id;
    s.x = p->x;
    s.y = p->y;
    s.hp = p->hp;
    s.max_hp = p->max_hp;
    s.floor_id = p->floor_id;
    s.is_merchant = 0;
    s.shop_spec = SHOP_SPEC_NONE;
    s.is_tombstone = 0;
    s.is_player = 1;
    copy_str(s.username, p->username, sizeof(s.username));
    if (wire_put_state(tx_frame(), &s)) {
        tx_send(sock);
    }
}

/*One frame with u16 count + 12-byte records instead of one MSG_STATE per
 * entity (100 known entities: ~1.2 KB).*/
static void flush_entity_batch(int sock, const EntityUpdateRec *recs, int count) {
    static MsgEntityUpdate m; /* 48 KiB: not on the stack */
    if (count <= 0 || count > WIRE_MAX_ENTITY_RECS) {
        return;
    }
    m.count = count;
    memcpy(m.recs, recs, sizeof(recs[0]) * (size_t)count);
    if (wire_put_entity_update(tx_frame(), &m)) {
        tx_send(sock);
    }
}

static bool within(int x, int y, const Client *c, int r2) {
    int ddx = x - c->x;
    int ddy = y - c->y;
    return ddx * ddx + ddy * ddy <= r2;
}

/*NPCs in range: a full state the first time the client sees one, then
 * 12-byte records batched in MSG_ENTITY_UPDATE.*/
static void send_nearby_npcs(Client *c, int c_idx, Game *game, int r2) {
    EntityUpdateRec recs[BATCH_ENTITY_CAP];
    int nrec = 0;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        const NPC *n = &game->npcs[i];
        if (!within(n->x, n->y, c, r2)) {
            continue;
        }
        if (!client_seen_test(c_idx, i)) {
            send_full_npc_state(c->sock, n);
            client_seen_set(c_idx, i);
            continue;
        }
        if (nrec >= BATCH_ENTITY_CAP) {
            flush_entity_batch(c->sock, recs, nrec);
            nrec = 0;
        }
        recs[nrec].entity_id = n->entity_id;
        recs[nrec].x = n->x;
        recs[nrec].y = n->y;
        recs[nrec].hp = n->hp; /*int32_t: boss HP can exceed int16*/
        nrec++;
    }
    if (nrec > 0) {
        flush_entity_batch(c->sock, recs, nrec);
    }
}

/*Tombstones in range, as entities with the negative id -(slot + 1).*/
static void send_nearby_tombstones(Game *game, Client *c, int r2) {
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        const Tombstone *t = &game->tombstones[i];
        if (!t->active || t->floor_id != c->floor_id || !within(t->x, t->y, c, r2)) {
            continue;
        }
        MsgState s;
        memset(&s, 0, sizeof(s));
        s.entity_id = -(i + 1);
        s.x = t->x;
        s.y = t->y;
        s.hp = 1;
        s.max_hp = 1;
        s.floor_id = t->floor_id;
        s.shop_spec = SHOP_SPEC_NONE;
        s.is_tombstone = 1;
        if (wire_put_state(tx_frame(), &s)) {
            tx_send(c->sock);
        }
    }
}

/*Other players in range: the same compact path as the NPCs.*/
static void send_nearby_players(Game *game, Client *c, int c_idx, int r2) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client *o = &game->clients[i];
        if (!o->active || !o->authenticated || o->floor_id != c->floor_id ||
            o->entity_id == c->entity_id || !within(o->x, o->y, c, r2)) {
            continue;
        }
        if (!player_seen_test(c_idx, i)) {
            send_full_player_state(c->sock, o);
            player_seen_set(c_idx, i);
        } else {
            EntityUpdateRec pr = {o->entity_id, o->x, o->y, o->hp};
            flush_entity_batch(c->sock, &pr, 1);
        }
    }
}

/*Sends the player what he can plausibly perceive: the entities within
 * the vision radius + 32 tiles (48 to 64). Sending the whole floor cost
 * ~450 KB per step on a full one, and the client discards what is beyond
 * its view anyway.*/
void broadcast_nearby_entities(Client *c, Game *game) {
    int radius = get_vision_radius(c) + 32;
    if (radius < 48) {
        radius = 48;
    }
    if (radius > 64) {
        radius = 64;
    }
    int c_idx = (int)(c - game->clients); /*c is always a member of game->clients*/
    if (c_idx < 0 || c_idx >= MAX_CLIENTS) {
        return;
    }
    client_seen_maybe_reset(c_idx, c->floor_id);
    send_nearby_npcs(c, c_idx, game, radius * radius);
    send_nearby_tombstones(game, c, radius * radius);
    send_nearby_players(game, c, c_idx, radius * radius);
}

void notify_player_left_floor(Game *game, Client *c, int old_floor) {
    int c_idx = (int)(c - game->clients); /*c is always a member of game->clients*/
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id = c->entity_id;
    s.x = c->x;
    s.y = c->y;
    s.hp = 0; // Client removes entities with hp <= 0
    s.max_hp = c->max_hp;
    s.floor_id = old_floor;
    s.is_merchant = 0;
    s.shop_spec = SHOP_SPEC_NONE;
    s.is_tombstone = 0;
    s.is_player = 1;
    copy_str(s.username, c->username, sizeof(s.username));
    if (!wire_put_state(tx_frame(), &s)) {
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (game->clients[i].active && game->clients[i].authenticated &&
            game->clients[i].floor_id == old_floor) {
            if (game->clients[i].entity_id == c->entity_id) {
                continue;
            }
            tx_send(game->clients[i].sock);
            /*The receiver just lost this entity (hp<=0 removal): clear the
       * "seen" bit so the NEXT time the two players share a floor the
       * state is sent in FULL. A compact update for an id the client
       * no longer caches is ignored by design, which would leave the
       * player invisible until the receiver changed floor.*/
            if (c_idx >= 0 && c_idx < MAX_CLIENTS) {
                player_seen_clear(i, c_idx);
            }
        }
    }
}

void broadcast_player_state(Game *game, Client *c) {
    int c_idx = (int)(c - game->clients); /*c is always a member of game->clients*/
    if (c_idx < 0 || c_idx >= MAX_CLIENTS) {
        return;
    }
    /*Compact record for receivers that already know this player: 22 bytes
   * on the wire (8 header + 2 count + 12 record) instead of a full
   * MsgState.*/
    EntityUpdateRec rec;
    rec.entity_id = c->entity_id;
    rec.x = c->x;
    rec.y = c->y;
    rec.hp = c->hp;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!game->clients[i].active || !game->clients[i].authenticated) {
            continue;
        }
        if (game->clients[i].floor_id != c->floor_id) {
            continue;
        }
        if (game->clients[i].entity_id == c->entity_id) {
            continue;
        }
        /*Stale bitmap (the receiver moved since its last full scan):
     * reset it so EVERYTHING — NPCs and players — is re-sent in full.*/
        client_seen_maybe_reset(i, game->clients[i].floor_id);
        if (!player_seen_test(i, c_idx)) {
            /*First time this client sees this player: full state (identity:
       * username, is_player, hp/max_hp, ...).*/
            send_full_player_state(game->clients[i].sock, c);
            player_seen_set(i, c_idx);
        } else {
            flush_entity_batch(game->clients[i].sock, &rec, 1);
        }
    }
}
