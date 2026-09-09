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
#include "protocol.h"
#include "map.h"
#include "game.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void send_text_to_client(int sock, const char *fmt, ...) {
  MsgHeader h = msg_hdr(MSG_TEXT, (int)sizeof(MsgText));
  MsgText m;
  va_list a;
  va_start(a, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, a);
  va_end(a);
  net_send_client(sock, &h, sizeof(h));
  net_send_client(sock, &m, sizeof(m));
}

/*--- Robust client send (L2) -----------------------------------------
 * net_send() gives up after ~1 s of retries (returns false) when the
 * peer's TCP buffer stays full or the connection died. Callers used to
 * ignore that result: the message was simply LOST, and the client sat
 * in a stale state (missing position/HP/combat update) until the next
 * full state — which, for the very message that got dropped, may never
 * come. A client that cannot receive is unusable: drop it on the FIRST
 * failed send. The server is the source of truth, so a reconnect
 * re-sends everything (welcome + full state + map + nearby entities)
 * and nothing is lost.
 *
 * Returns true if the whole payload was queued; false if the client
 * was marked for removal — callers must not rely on further sends to
 * it in the same tick. That is cheap anyway: net_send() fails fast on
 * permanent errors (closed fd, dead peer), so post-drop sends do not
 * stall the loop for the ~1 s retry budget.*/
bool net_send_client(int sock, const void *data, int len) {
  if (net_send(sock, data, len))
    return true;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].sock == sock) {
      if (g_clients[i].authenticated)
        save_player_data(&g_clients[i]);
      server_log("NET", "send failed: disconnecting client %d ('%s')",
                 i, g_clients[i].username);
      /*Same cleanup as the read-side disconnect path in main(): tell
       * the other players on the floor that this one left (hp<=0
       * removal), so its entity does not linger on their screens.*/
      notify_player_left_floor(&g_clients[i], g_clients[i].floor_id);
      net_close(sock);
      g_clients[i].sock = -1;
      g_clients[i].active = false;
      return false;
    }
  }
  /*Socket not mapped to a live client (already reaped this tick):
   * nothing to fix up.*/
  return false;
}

void send_map_chunk(int sock, Map *map, int cx, int cy, int size) {
  MsgHeader h;
  MsgMapChunk mc;
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
  h = msg_hdr(MSG_MAP_CHUNK,
              (int)(sizeof(mc) + (size_t)size * size * sizeof(VoxelType)));
  VoxelType *buf = malloc(size * size * sizeof(VoxelType));
  if (!buf)
    return;
  int idx = 0;
  for (int iy = sy; iy < sy + size; iy++)
    for (int ix = sx; ix < sx + size; ix++)
      buf[idx++] = map->data[0][iy][ix];
  net_send_client(sock, &h, sizeof(h));
  net_send_client(sock, &mc, sizeof(mc));
  net_send_client(sock, buf, size * size * sizeof(VoxelType));
  free(buf);
}

void send_detailed_state(Client *c) {
  MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id = c->entity_id;
  s.x = c->x;
  s.y = c->y;
  copy_str(s.username, c->username, sizeof(s.username));
  s.hp = c->hp;
  s.max_hp = c->max_hp;
  s.floor_id = c->floor_id;
  get_game_time(&s.game_hour, &s.game_min);
  s.total_turns = global_total_turns;
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
  // Weapon name
  if (c->slot_hand_r.template_idx != -1) {
    copy_str(s.weapon_name, item_database[c->slot_hand_r.template_idx].name, sizeof(s.weapon_name));
  } else if (c->slot_hand_l.template_idx != -1) {
    copy_str(s.weapon_name, item_database[c->slot_hand_l.template_idx].name, sizeof(s.weapon_name));
  } else {
    copy_str(s.weapon_name, "Bare Hands", sizeof(s.weapon_name));
  }
  // Armor name
  if (c->slot_body.template_idx != -1) {
    copy_str(s.armor_name, item_database[c->slot_body.template_idx].name, sizeof(s.armor_name));
  } else {
    copy_str(s.armor_name, "None", sizeof(s.armor_name));
  }
  // Combat bonuses
  int ts2, td2, tc2, ti2, tw2, th2;
  get_total_stats(c, &ts2, &td2, &tc2, &ti2, &tw2, &th2);
  s.to_dmg = rules_get_modifier(ts2);
  {
    const ItemTemplate *w2 = NULL;
    if (c->slot_hand_r.template_idx != -1)
      w2 = &item_database[c->slot_hand_r.template_idx];
    else if (c->slot_hand_l.template_idx != -1)
      w2 = &item_database[c->slot_hand_l.template_idx];
    int a_mod = (w2 && w2->damage_dice_sides <= 6) ? rules_get_modifier(td2)
                                                   : rules_get_modifier(ts2);
    s.to_hit = (w2 ? w2->attack_bonus : 0) + a_mod + (c->level / 2);
  }

  //--- POPULATION OF STATUS ICONS (BITMASK) ---
  s.status_icons = 0;
  if (rules_has_condition_t(c->effects, c->effect_count, COND_POISONED))
    s.status_icons |= (1 << 0);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_BLINDED))
    s.status_icons |= (1 << 1);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_PARALYZED))
    s.status_icons |= (1 << 2);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_STUNNED))
    s.status_icons |= (1 << 3);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_UNCONSCIOUS))
    s.status_icons |= (1 << 4);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_BURNING))
    s.status_icons |= (1 << 5);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_BLEEDING))
    s.status_icons |= (1 << 6);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_PETRIFIED))
    s.status_icons |= (1 << 7);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_CURSED))
    s.status_icons |= (1 << 8);
  if (rules_has_condition_t(c->effects, c->effect_count, COND_FROZEN))
    s.status_icons |= (1 << 9);
  if (c->exhaustion_level > 0)
    s.status_icons |= (1 << 10);
  if (c->needs_study)
    s.status_icons |= (1 << 11);
  
  s.bosses_defeated = c->bosses_defeated;
  s.hunger_level = c->hunger_level;
  for (int i = 0; i < 10; i++) {
      s.spell_slots[i] = c->spell_slots[i];
      s.spell_slots_max[i] = c->spell_slots_max[i];
  }

  /* Slot equipment names */
  #define SLOT_NAME(slot_field, dest) \
    do { \
      if ((slot_field).template_idx >= 0 && (slot_field).template_idx < item_database_size) { \
          if ((slot_field).is_artifact) \
              copy_str(dest, (slot_field).artifact_name, 32); \
          else \
              copy_str(dest, item_database[(slot_field).template_idx].name, 32); \
          dest[31] = '\0'; \
      } else { \
          dest[0] = '\0'; \
      } \
    } while(0)

  SLOT_NAME(c->slot_head,   s.eq_head);
  SLOT_NAME(c->slot_neck,   s.eq_neck);
  SLOT_NAME(c->slot_body,   s.eq_body);
  SLOT_NAME(c->slot_back,   s.eq_back);
  SLOT_NAME(c->slot_hand_r, s.eq_hand_r);
  SLOT_NAME(c->slot_hand_l, s.eq_hand_l);
  SLOT_NAME(c->slot_hands,  s.eq_hands);
  SLOT_NAME(c->slot_arm_r,  s.eq_arm_r);
  SLOT_NAME(c->slot_arm_l,  s.eq_arm_l);
  SLOT_NAME(c->slot_feet,   s.eq_feet);
  for (int i = 0; i < 10; i++) {
      SLOT_NAME(c->slot_rings[i], s.eq_ring[i]);
  }
  for (int i = 0; i < 4; i++) {
      SLOT_NAME(c->belt[i], s.eq_belt[i]);
  }
  #undef SLOT_NAME

  net_send_client(c->sock, &h, sizeof(h));
  net_send_client(c->sock, &s, sizeof(s));
}

void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id) {
    MsgHeader hdr = msg_hdr(MSG_SPELL_VFX, (int)sizeof(MsgSpellVFX));
    
    MsgSpellVFX msg;
    msg.start_x = sx; msg.start_y = sy;
    msg.target_x = tx; msg.target_y = ty;
    msg.vfx_type = vfx_type;
    msg.color_r = r; msg.color_g = g; msg.color_b = b;
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == floor_id) {
            /*net_send_client() (NOT raw write()): on a non-blocking socket a
             * partial write() desynced that client's whole protocol
             * stream — every later message became garbage until the
             * connection died. net_send loops until the full payload
             * is queued; net_send_client additionally DROPS the client
             * if the send fails (a message it never receives would
             * leave it in a stale state — see net_send_client).*/
            net_send_client(g_clients[i].sock, &hdr, sizeof(MsgHeader));
            net_send_client(g_clients[i].sock, &msg, sizeof(MsgSpellVFX));
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
static int     g_client_seen_floor[MAX_CLIENTS]; /*floor+1, 0 = empty*/

/*--- Per-client "seen PLAYERS" bitmap (L9) ---------------------------
 * broadcast_player_state() used to ship a ~1 KB full MsgState to EVERY
 * player on the floor on EVERY step: 60 players on one plane meant
 * ~60 KB per single step, mostly redundant (the client already knows
 * the player's identity — it only needs x/y/hp). Players now ride the
 * SAME compact path as NPCs: a full MsgState the first time a given
 * client sees a given player, then 12-byte EntityUpdateRec records.
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
  g_client_seen_players[ci][pi >> 3] &= (uint8_t)~(1u << (pi & 7));
}
static inline void client_seen_maybe_reset(int ci, int floor) {
  if (g_client_seen_floor[ci] != floor + 1)
    client_seen_reset(ci, floor);
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
  MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id   = n->entity_id;
  s.x           = n->x;
  s.y           = n->y;
  s.hp          = n->hp;
  s.max_hp      = n->max_hp;
  s.floor_id    = n->floor_id;
  s.is_merchant = (n->archetype == ARCH_MERCHANT) ? 1 : 0;
  s.shop_spec = (n->archetype == ARCH_MERCHANT)
                    ? (int)n->merchant.spec
                    : SHOP_SPEC_NONE;
  s.is_tombstone = 0;
  s.is_player = 0;
  net_send_client(sock, &h, sizeof(h));
  net_send_client(sock, &s, sizeof(s));
}

/*Full MsgState for a PLAYER (identity: username, is_player, hp/max_hp).
 * Sent the first time a client sees that player; afterwards the client
 * receives 12-byte EntityUpdateRec records (see broadcast_player_state
 * and the player section of broadcast_nearby_entities).*/
static void send_full_player_state(int sock, const Client *p) {
  MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id   = p->entity_id;
  s.x           = p->x;
  s.y           = p->y;
  s.hp          = p->hp;
  s.max_hp      = p->max_hp;
  s.floor_id    = p->floor_id;
  s.is_merchant = 0;
  s.shop_spec   = SHOP_SPEC_NONE;
  s.is_tombstone= 0;
  s.is_player   = 1;
  copy_str(s.username, p->username, sizeof(s.username));
  net_send_client(sock, &h, sizeof(h));
  net_send_client(sock, &s, sizeof(s));
}

/*One header + [int32 count][records] instead of N header+MsgState pairs.
 * 100 known entities: ~150 KB (old way) -> ~1.2 KB.*/
static void flush_entity_batch(int sock, const EntityUpdateRec *recs, int count) {
  if (count <= 0) return;
  MsgHeader h = msg_hdr(MSG_ENTITY_UPDATE,
                        (int)(sizeof(int32_t) + (size_t)count * sizeof(EntityUpdateRec)));
  int32_t cnt = count;
  net_send_client(sock, &h, sizeof(h));
  net_send_client(sock, &cnt, sizeof(cnt));
  net_send_client(sock, recs, (size_t)count * sizeof(EntityUpdateRec));
}

void broadcast_nearby_entities(Client *c, NPC *npcs) {
  /*Send only entities the player can plausibly perceive:
  * Euclidean distance <= vision radius + margin.
  * Before the distance filter the function shipped EVERY active NPC on
  * the floor (~300+ MsgState at ~1.5 KB each, ~450 KB per single step on
  * a full floor) even though the client discards anything beyond its own
  * view. On top of the filter, entities are sent INCREMENTALLY: a full
  * MsgState only the first time the client sees them, then 12-byte
  * compact records batched in a single MSG_ENTITY_UPDATE. The scan walks
  * the per-floor index (O(entities on the floor)) with a full-scan
  * fallback for an overflowed floor.*/
  int vr = get_vision_radius(c);
  int radius = vr + 32;
  if (radius < 48) radius = 48;
  if (radius > 64) radius = 64;
  int r2 = radius * radius;

  int c_idx = (int)(c - g_clients); /*c is always a g_clients member*/
  if (c_idx < 0 || c_idx >= MAX_CLIENTS) return;
  client_seen_maybe_reset(c_idx, c->floor_id);

  EntityUpdateRec recs[BATCH_ENTITY_CAP];
  int nrec = 0;

  int idx_n = 0;
  const int *fidx = floor_index_for(c->floor_id, &idx_n);
  int n_slots = (fidx != NULL) ? idx_n : MAX_NPCS;

  /*--- Active NPCs ---*/
  for (int k = 0; k < n_slots; k++) {
    int i = (fidx != NULL) ? fidx[k] : k;
    const NPC *n = &npcs[i];
    if (!n->active || n->floor_id != c->floor_id) continue; /*stale index entry*/
    int ddx = n->x - c->x;
    int ddy = n->y - c->y;
    if (ddx * ddx + ddy * ddy > r2) continue;
    if (!client_seen_test(c_idx, i)) {
      send_full_npc_state(c->sock, n);
      client_seen_set(c_idx, i);
    } else {
      if (nrec >= BATCH_ENTITY_CAP) {
        flush_entity_batch(c->sock, recs, nrec);
        nrec = 0;
      }
      recs[nrec].entity_id = n->entity_id;
      recs[nrec].x = (int16_t)n->x;
      recs[nrec].y = (int16_t)n->y;
      recs[nrec].hp = n->hp; /*int32_t: boss HP can exceed int16 (M6)*/
      nrec++;
    }
  }
  if (nrec > 0) {
    flush_entity_batch(c->sock, recs, nrec);
  }
  
  /*--- Active Tombstones on the same plane ---*/
  for (int i = 0; i < MAX_TOMBSTONES; i++) {
    if (!g_tombstones[i].active) continue;
    if (g_tombstones[i].floor_id != c->floor_id) continue;
    {
      int ddx = g_tombstones[i].x - c->x;
      int ddy = g_tombstones[i].y - c->y;
      if (ddx * ddx + ddy * ddy > r2) continue;
    }
    int tomb_id = -(i + 1);
    MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id    = tomb_id;
    s.x            = g_tombstones[i].x;
    s.y            = g_tombstones[i].y;
    s.hp           = 1;
    s.max_hp       = 1;
    s.floor_id     = g_tombstones[i].floor_id;
    s.is_merchant  = 0;
    s.shop_spec    = SHOP_SPEC_NONE;
    s.is_tombstone = 1;
    s.is_player    = 0;
    net_send_client(c->sock, &h, sizeof(h));
    net_send_client(c->sock, &s, sizeof(s));
  }
  
  /*--- Active Players on the same plane (L9: the same compact path as
   * NPCs — full state only the first time the client sees them, then
   * 12-byte records — player IDs are positive and unique like NPC IDs) ---*/
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == c->floor_id) {
      if (g_clients[i].entity_id == c->entity_id) continue;
      {
        int ddx = g_clients[i].x - c->x;
        int ddy = g_clients[i].y - c->y;
        if (ddx * ddx + ddy * ddy > r2) continue;
      }
      if (!player_seen_test(c_idx, i)) {
        send_full_player_state(c->sock, &g_clients[i]);
        player_seen_set(c_idx, i);
      } else {
        EntityUpdateRec pr;
        pr.entity_id = g_clients[i].entity_id;
        pr.x = (int16_t)g_clients[i].x;
        pr.y = (int16_t)g_clients[i].y;
        pr.hp = g_clients[i].hp;
        flush_entity_batch(c->sock, &pr, 1);
      }
    }
  }
}




void notify_player_left_floor(Client *c, int old_floor) {
  int c_idx = (int)(c - g_clients); /*c is always a g_clients member*/
  MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id   = c->entity_id;
  s.x           = c->x;
  s.y           = c->y;
  s.hp          = 0; // Client removes entities with hp <= 0
  s.max_hp      = c->max_hp;
  s.floor_id    = old_floor;
  s.is_merchant = 0;
  s.shop_spec   = SHOP_SPEC_NONE;
  s.is_tombstone= 0;
  s.is_player   = 1;
  copy_str(s.username, c->username, sizeof(s.username));
  
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == old_floor) {
      if (g_clients[i].entity_id == c->entity_id) continue;
      net_send_client(g_clients[i].sock, &h, sizeof(h));
      net_send_client(g_clients[i].sock, &s, sizeof(s));
      /*The receiver just lost this entity (hp<=0 removal): clear the
       * "seen" bit so the NEXT time the two players share a floor the
       * state is sent in FULL. A compact update for an id the client
       * no longer caches is ignored by design, which would leave the
       * player invisible until the receiver changed floor.*/
      if (c_idx >= 0 && c_idx < MAX_CLIENTS)
        player_seen_clear(i, c_idx);
    }
  }
}

void broadcast_player_state(Client *c) {
  int c_idx = (int)(c - g_clients); /*c is always a g_clients member*/
  if (c_idx < 0 || c_idx >= MAX_CLIENTS)
    return;
  /*Compact record for receivers that ALREADY know this player (L9).
   * 24 bytes on the wire (8 header + 4 count + 12 record) instead of
   * the ~1.5 KB full MsgState the old code sent to every player on
   * the floor on every step.*/
  EntityUpdateRec rec;
  rec.entity_id = c->entity_id;
  rec.x = (int16_t)c->x;
  rec.y = (int16_t)c->y;
  rec.hp = c->hp;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!g_clients[i].active || !g_clients[i].authenticated)
      continue;
    if (g_clients[i].floor_id != c->floor_id)
      continue;
    if (g_clients[i].entity_id == c->entity_id)
      continue;
    /*Stale bitmap (the receiver moved since its last full scan):
     * reset it so EVERYTHING — NPCs and players — is re-sent in full.*/
    client_seen_maybe_reset(i, g_clients[i].floor_id);
    if (!player_seen_test(i, c_idx)) {
      /*First time this client sees this player: full state (identity:
       * username, is_player, hp/max_hp, ...).*/
      send_full_player_state(g_clients[i].sock, c);
      player_seen_set(i, c_idx);
    } else {
      flush_entity_batch(g_clients[i].sock, &rec, 1);
    }
  }
}

