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
 * Network thread: reads the server's frames and applies them to the
 * shared client state.
 *
 * The socket is non-blocking; the thread waits with poll() (100 ms, so it
 * notices g_running going false), appends what arrived to a receive
 * buffer and handles every complete frame in it. A frame is never read
 * piecemeal with sleeps and retries, so a slow link cannot desynchronize
 * the stream. Any malformed frame (wrong version, a type or length the
 * server may not send, a payload wire.c rejects) ends the session with a
 * message: the stream can no longer be trusted.
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client_fct.h"
#include "client_state.h"
/*Atomic: shared between the net thread (sets, under g_state_mutex) and
 * the VK render thread (reads/clears, without the lock) — see the
 * declaration in client_state.h.*/
atomic_bool g_map_dirty = false;
#include "client_minimap.h"
#include "client_particles.h"
#include "net.h"
#include "protocol.h"
#include "wire.h"

static void session_end(const char *fmt, const char *detail) {
    printf("\n[NET] ");
    printf(fmt, detail);
    printf("\n");
    fflush(stdout);
    g_running = false;
}

/* --- Handlers (payload already length-checked against the type) --------- */

static void on_welcome(const MsgWelcome *w) {
    pthread_mutex_lock(&g_state_mutex);
    g_my_entity_id = w->entity_id;
    g_my_x = w->x;
    g_my_y = w->y;
    g_my_hp = w->hp;
    g_my_max_hp = w->max_hp;
    g_my_gold = w->gold;
    g_my_level = w->level;
    g_race_id = w->race_id;
    g_subrace_id = w->subrace_id;
    g_class_id = w->class_id;
    g_alignment = w->alignment;
    pthread_mutex_unlock(&g_state_mutex);
    printf("\n[NET] Authentication completed!\n[STAT] HP: %d/%d | Gold: %lu\n>", w->hp,
           w->max_hp, (unsigned long)w->gold);
    fflush(stdout);
}

static void set_entity(Entity *e, const MsgState *s) {
    e->id = s->entity_id;
    e->x = s->x;
    e->y = s->y;
    e->hp = s->hp;
    e->floor_id = s->floor_id;
    e->active = true;
    e->is_merchant = (s->is_merchant != 0);
    e->shop_spec = s->shop_spec;
    e->is_tombstone = (s->is_tombstone != 0);
    e->is_player = (s->is_player != 0);
    copy_str(e->username, s->username, sizeof(e->username));
}

static void on_state(const MsgState *s) {
    static int last_floor = -1;
    pthread_mutex_lock(&g_state_mutex);
    if (last_floor != -1 && last_floor != s->floor_id) {
        for (int ly = 0; ly < MAP_HEIGHT; ly++)
            for (int lx = 0; lx < MAP_WIDTH; lx++)
                g_local_map[ly][lx] = VOXEL_ROCK;
        /*Floor change: every cached entity belongs to the old floor.*/
        for (int li = 0; li < CLIENT_MAX_ENTITIES; li++) {
            g_entities[li].active = false;
            g_entities[li].id = 0;
            g_entities[li].floor_id = -1;
        }
        minimap_reset();
    }
    last_floor = s->floor_id;
    if (s->entity_id == g_my_entity_id) {
        g_my_x = s->x;
        g_my_y = s->y;
        g_my_hp = s->hp;
        g_my_max_hp = s->max_hp;
        g_game_h = s->game_hour;
        g_game_m = s->game_min;
        g_total_turns = s->total_turns;
        g_str = s->str;
        g_dex = s->dex;
        g_con = s->con;
        g_intel = s->intel;
        g_wis = s->wis;
        g_cha = s->cha;
        g_movement_cooldown = s->movement_cooldown;
        g_equipped_mask = s->equipped_mask;
        g_my_level = s->level;
        g_my_xp = s->xp;
        g_my_gold = s->gold;
        g_my_ac = s->ac;
        g_my_floor = s->floor_id;
        g_vision_radius = s->vision_radius;
        copy_str(g_weapon_name, s->weapon_name, sizeof(g_weapon_name));
        copy_str(g_armor_name, s->armor_name, sizeof(g_armor_name));
        g_to_hit = s->to_hit;
        g_to_dmg = s->to_dmg;
        g_bosses_defeated = s->bosses_defeated;
        g_status_icons = s->status_icons;
        g_hunger_level = s->hunger_level;
        for (int i = 0; i < 10; i++) {
            g_my_spell_slots[i] = s->spell_slots[i];
            g_my_spell_slots_max[i] = s->spell_slots_max[i];
        }
        copy_str(g_eq_head, s->eq_head, sizeof(g_eq_head));
        copy_str(g_eq_neck, s->eq_neck, sizeof(g_eq_neck));
        copy_str(g_eq_body, s->eq_body, sizeof(g_eq_body));
        copy_str(g_eq_back, s->eq_back, sizeof(g_eq_back));
        copy_str(g_eq_hand_r, s->eq_hand_r, sizeof(g_eq_hand_r));
        copy_str(g_eq_hand_l, s->eq_hand_l, sizeof(g_eq_hand_l));
        copy_str(g_eq_hands, s->eq_hands, sizeof(g_eq_hands));
        copy_str(g_eq_arm_r, s->eq_arm_r, sizeof(g_eq_arm_r));
        copy_str(g_eq_arm_l, s->eq_arm_l, sizeof(g_eq_arm_l));
        copy_str(g_eq_feet, s->eq_feet, sizeof(g_eq_feet));
        for (int i = 0; i < 10; i++)
            copy_str(g_eq_ring[i], s->eq_ring[i], sizeof(g_eq_ring[0]));
        for (int i = 0; i < 4; i++)
            copy_str(g_eq_belt[i], s->eq_belt[i], sizeof(g_eq_belt[0]));
    } else if (s->floor_id == g_my_floor) {
        /*An NPC, tombstone or other player. hp <= 0: it died or left this
         * floor (stairs, teleport, disconnect), so it is removed.*/
        Entity *slot = NULL;
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
            if (g_entities[i].id == s->entity_id) {
                slot = &g_entities[i];
                break;
            }
        }
        if (s->hp <= 0) {
            if (slot) {
                slot->active = false;
                slot->id = 0;
                slot->floor_id = -1;
            }
        } else {
            /*FEATURE LIMIT (see CLIENT_MAX_ENTITIES in client_state.h):
             * with every cache slot in use a new entity is not drawn on
             * this client. The cache is flushed on every floor change.*/
            for (int i = 0; !slot && i < CLIENT_MAX_ENTITIES; i++)
                if (!g_entities[i].active)
                    slot = &g_entities[i];
            if (slot)
                set_entity(slot, s);
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
}

/*Positions and HP of entities the server already sent a full MSG_STATE
 * for; an unknown id is ignored by design (its full state comes first).*/
static void on_entity_update(const EntityUpdateRec *recs, int count) {
    pthread_mutex_lock(&g_state_mutex);
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
            if (g_entities[i].id == recs[k].entity_id) {
                g_entities[i].x = recs[k].x;
                g_entities[i].y = recs[k].y;
                g_entities[i].hp = recs[k].hp;
                g_entities[i].active = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static void on_map_chunk(const MsgMapChunk *mc, const uint8_t *tiles) {
    pthread_mutex_lock(&g_state_mutex);
    /*The server re-sends the player-centered window on every step: mark
     * the map dirty (mesh rebuild) only if at least one tile changed.*/
    bool changed = false;
    int i = 0;
    for (int cy = mc->start_y; cy < mc->start_y + mc->height; cy++) {
        for (int cx = mc->start_x; cx < mc->start_x + mc->width; cx++) {
            if (g_local_map[cy][cx] != tiles[i]) {
                g_local_map[cy][cx] = tiles[i];
                changed = true;
            }
            i++;
        }
    }
    if (changed)
        g_map_dirty = true;
    pthread_mutex_unlock(&g_state_mutex);
}

static void on_tombstone_remove(int entity_id) {
    pthread_mutex_lock(&g_state_mutex);
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        if (g_entities[i].active && g_entities[i].id == entity_id &&
            g_entities[i].is_tombstone) {
            g_entities[i].active = false;
            g_entities[i].is_tombstone = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
}

/* Decode and apply one frame. False = malformed payload. */
static bool handle_frame(MsgType type, const uint8_t *p, size_t n) {
    switch (type) {
    case MSG_WELCOME: {
        MsgWelcome w;
        if (!wire_get_welcome(p, n, &w))
            return false;
        on_welcome(&w);
        return true;
    }
    case MSG_AUTH_FAIL: {
        MsgAuthFail f;
        if (!wire_get_auth_fail(p, n, &f))
            return false;
        session_end("Authentication failed: %s", f.reason);
        return true;
    }
    case MSG_STATE: {
        static MsgState s; /* ~1.3 KB: not on the thread's stack every time */
        if (!wire_get_state(p, n, &s))
            return false;
        on_state(&s);
        return true;
    }
    case MSG_ENTITY_UPDATE: {
        static EntityUpdateRec recs[WIRE_MAX_ENTITY_RECS];
        int count = 0;
        if (!wire_get_entity_update(p, n, recs, WIRE_MAX_ENTITY_RECS, &count))
            return false;
        on_entity_update(recs, count);
        return true;
    }
    case MSG_MAP_CHUNK: {
        MsgMapChunk mc;
        const uint8_t *tiles = NULL;
        if (!wire_get_map_chunk(p, n, &mc, &tiles))
            return false;
        on_map_chunk(&mc, tiles);
        return true;
    }
    case MSG_SPELL_VFX: {
        MsgSpellVFX v;
        if (!wire_get_spell_vfx(p, n, &v))
            return false;
        spawn_vfx(v.vfx_type, (float)v.start_x, (float)v.start_y, (float)v.target_x,
                  (float)v.target_y, v.color_r, v.color_g, v.color_b);
        return true;
    }
    case MSG_TEXT: {
        MsgText t;
        if (!wire_get_text(p, n, &t))
            return false;
        client_log_add(t.text);
        printf("\r\033[K%s\n> ", t.text);
        fflush(stdout);
        return true;
    }
    case MSG_COMBAT_EVENT: {
        MsgCombatEvent ev;
        if (!wire_get_combat_event(p, n, &ev))
            return false;
        fct_combat_event(ev.kind, ev.amount);
        return true;
    }
    case MSG_TIME_SYNC: {
        MsgTimeSync ts;
        if (!wire_get_time_sync(p, n, &ts))
            return false;
        pthread_mutex_lock(&g_state_mutex);
        g_game_h = ts.game_hour;
        g_game_m = ts.game_min;
        g_total_turns = ts.total_turns;
        pthread_mutex_unlock(&g_state_mutex);
        return true;
    }
    case MSG_TOMBSTONE_REMOVE: {
        MsgTombstoneRemove rm;
        if (!wire_get_tombstone_remove(p, n, &rm))
            return false;
        on_tombstone_remove(rm.entity_id);
        return true;
    }
    default:
        return false;
    }
}

/* Handle every complete frame at the start of buf; returns the bytes
 * consumed, or -1 if the stream is broken. */
static long consume_frames(const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (g_running && len - off >= WIRE_HEADER_SIZE) {
        MsgHeader h;
        wire_read_header(buf + off, &h);
        if (h.version != PROTOCOL_VERSION) {
            char v[48];
            snprintf(v, sizeof(v), "%u (this client speaks %u)", (unsigned)h.version,
                     (unsigned)PROTOCOL_VERSION);
            session_end("Protocol error: the server speaks protocol version %s.", v);
            return -1;
        }
        uint32_t max = wire_server_payload_max(h.type);
        if (max == 0 || h.length > max) {
            char d[48];
            snprintf(d, sizeof(d), "type %u, length %u", (unsigned)h.type, (unsigned)h.length);
            session_end("Protocol error: unexpected message (%s).", d);
            return -1;
        }
        if (len - off < WIRE_HEADER_SIZE + (size_t)h.length)
            break; /* the rest of this frame has not arrived yet */
        if (!handle_frame(h.type, buf + off + WIRE_HEADER_SIZE, h.length)) {
            char d[24];
            snprintf(d, sizeof(d), "type %u", (unsigned)h.type);
            session_end("Protocol error: malformed message (%s).", d);
            return -1;
        }
        off += WIRE_HEADER_SIZE + (size_t)h.length;
    }
    return (long)off;
}

void *net_thread_loop(void *arg) {
    (void)arg;
    static uint8_t rx[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];
    size_t rx_len = 0;
    while (g_running) {
        struct pollfd pfd = {.fd = g_server_sock, .events = POLLIN};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            session_end("Connection to the server lost (%s).", strerror(errno));
            break;
        }
        if (ready == 0)
            continue;
        ssize_t got = recv(g_server_sock, rx + rx_len, sizeof(rx) - rx_len, 0);
        if (got == 0) {
            session_end("%s", "Server disconnected.");
            break;
        }
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            session_end("Connection to the server lost (%s).", strerror(errno));
            break;
        }
        rx_len += (size_t)got;
        long used = consume_frames(rx, rx_len);
        if (used < 0)
            break;
        memmove(rx, rx + used, rx_len - (size_t)used);
        rx_len -= (size_t)used;
    }
    return NULL;
}
