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
 * server_login.c - Connections before they are players: accepting, the login in two
 * halves around the login worker, refusals.
 */

#include "server_login.h"
#include "alignment.h"
#include "wire.h"
#include "server_conn.h"
#include "tls_common.h"
#include "login_worker.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "server_internal.h"

#include "player.h"
#include "server_log.h"

/*Username policy: [A-Za-z0-9_]{1,16}.
 * The username ends up in file names (saves/<name>.save,
 * saves/tombstone_<name>_*.dat): anything else (path separators, "..",
 * empty) is a traversal vector, so reject it at login.*/
static bool valid_username(const char *u) {
    if (!u) {
        return false;
    }
    size_t n = strlen(u);
    if (n < 1 || n > 16) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = u[i];
        if (!isalpha((unsigned char)ch) && !isdigit((unsigned char)ch) && ch != '_') {
            return false;
        }
    }
    return true;
}

/*The log line of a refused login (the worker never logs: the client may
 * already be gone, and the slot decides what the user is told).*/
/*--- Connection management (main loop only) ---------------------------*/

/*Refuse a login: queue the reason, forget any authentication state set
 * so far — the reaper must NEVER save a character whose password was
 * wrong — and mark the connection for removal.*/
static void reject_login(Client *c, int slot, const char *reason, bool count_failure) {
    send_auth_fail(c->sock, reason);
    c->authenticated = false;
    c->is_dm = false;
    if (count_failure) {
        ipguard_login_failed(conn_peer_ip(slot), get_time_ms());
    }
    conn_mark_drop(slot, reason);
}

/*The listening socket is dual-stack (net_create_server): an IPv4 client
 * arrives as ::ffff:a.b.c.d and is shown — and rate-limited by the IP
 * guard — under its plain IPv4 address.*/
static void peer_ip_string(const struct sockaddr_storage *ss, char *out, size_t out_len) {
    const void *addr = NULL;
    int family = ss->ss_family;
    struct in_addr v4;
    if (family == AF_INET) {
        addr = &((const struct sockaddr_in *)ss)->sin_addr;
    } else if (family == AF_INET6) {
        const struct in6_addr *a6 = &((const struct sockaddr_in6 *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a6)) {
            memcpy(&v4, &a6->s6_addr[12], sizeof(v4));
            addr = &v4;
            family = AF_INET;
        } else {
            addr = a6;
        }
    }
    if (!addr || !inet_ntop(family, addr, out, (socklen_t)out_len)) {
        copy_str(out, "?", out_len);
    }
}

/*In play, or with a login of that name in the login worker.*/
static bool character_online(const Client *clients, const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            continue;
        }
        if (clients[i].authenticated && strcmp(clients[i].username, username) == 0) {
            return true;
        }
        if (clients[i].login_pending && strcmp(clients[i].login_user, username) == 0) {
            return true;
        }
    }
    return false;
}

/*TCP keepalive: a client that vanished without closing (power loss,
 * dropped Wi-Fi) is detected after ~90 s, its slot freed and its
 * character saved and released for the next login.*/
static void enable_keepalive(int fd) {
    int on = 1, idle = 60, interval = 10, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

static int pending_logins_from(const Client *clients, const char *ip) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && !clients[i].authenticated && strcmp(conn_peer_ip(i), ip) == 0) {
            n++;
        }
    }
    return n;
}

/*Accept every queued connection (the listening socket is non-blocking).
 * A connection that cannot be served — address locked out, too many
 * pending logins from it, server full — gets a MSG_AUTH_FAIL and is
 * closed at once: it never occupies a slot or leaks its descriptor.*/
void accept_pending_connections(int listen_fd, int epoll_fd, Client *clients) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int cs = accept(listen_fd, (struct sockaddr *)&ss, &ss_len);
        if (cs >= 0) {
            net_set_nonblocking(cs);
            fcntl(cs, F_SETFD, FD_CLOEXEC); /*not inherited by the PDF generator*/
            enable_keepalive(cs);
        }
        if (cs < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                server_log("NET", "accept failed: %s", strerror(errno));
            }
            return;
        }
        char ip[CONN_IP_MAX];
        peer_ip_string(&ss, ip, sizeof(ip));
        long long now = get_time_ms();

        const char *refusal = NULL;
        int slot = -1;
        if (ipguard_locked(ip, now)) {
            refusal = "Too many failed logins from your address: try again later.";
        } else if (pending_logins_from(clients, ip) >= MAX_PENDING_PER_IP) {
            refusal = "Too many pending connections from your address.";
        } else {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!clients[i].active) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                refusal = "Server full: try again later.";
            }
        }
        if (refusal) {
            server_log("NET", "Connection from %s refused: %s", ip, refusal);
            conn_refuse(epoll_fd, cs, ip, refusal, now);
            continue;
        }

        /*A slot starts from a clean Client: nothing of its previous
     * occupant (username, trade state, DM flag...) may leak in.*/
        memset(&clients[slot], 0, sizeof(clients[slot]));
        clients[slot].sock = cs;
        clients[slot].active = true;
        clients[slot].trade_offer_item_idx = -1;
        clients[slot].pending_trade_merchant_id = -1;
        if (!conn_open(slot, cs, ip, now)) {
            char why[128];
            tls_error_text(why, sizeof(why), "out of memory");
            server_log("NET", "Cannot start TLS for %s: %s", ip, why);
            clients[slot].active = false;
            close(cs);
            continue;
        }
        client_seen_reset(slot, -1);

        struct epoll_event client_ev;
        memset(&client_ev, 0, sizeof(client_ev));
        client_ev.events = EPOLLIN | EPOLLRDHUP;
        client_ev.data.u32 = (uint32_t)slot;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cs, &client_ev) != 0) {
            server_log("NET", "epoll_ctl(ADD) failed for %s: %s", ip, strerror(errno));
            conn_release(slot);
            clients[slot].active = false;
            close(cs);
            continue;
        }
        server_log("NET", "Client %d connected from %s", slot, ip);
    }
}

/*A connection must log in within LOGIN_TIMEOUT_MS of being accepted.*/
void expire_pending_logins(Client *clients) {
    long long now = get_time_ms();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && !clients[i].authenticated && !conn_dropping(i) &&
            now - conn_opened_ms(i) > LOGIN_TIMEOUT_MS) {
            send_auth_fail(clients[i].sock, "Login timeout.");
            conn_mark_drop(i, "login timeout");
        }
    }
}

/*Server access password (required) and DM password (optional), from a
 * file or the environment. Only digests are kept; the plaintext is wiped.*/
bool load_server_secrets(const char *gate_file, const char *dm_file) {
    char secret[128], err[256];
    int rc = auth_load_secret(gate_file, "DRAGONGL_SERVER_PASSWORD", secret, sizeof(secret), err,
                              sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "[SYS] Server password: %s\n", err);
        return false;
    }
    if (rc == 0) {
        fprintf(stderr, "[SYS] No server password: set DRAGONGL_SERVER_PASSWORD or use "
                        "--password-file <file>.\n");
        return false;
    }
    if (err[0]) {
        fprintf(stderr, "[SYS] WARNING: %s\n", err);
    }
    const char *why = auth_secret_problem(secret);
    if (why) {
        auth_wipe(secret, sizeof(secret));
        fprintf(stderr, "[SYS] Server password %s.\n", why);
        return false;
    }
    auth_set_gate_secret(secret);
    auth_wipe(secret, sizeof(secret));

    rc =
        auth_load_secret(dm_file, "DRAGONGL_DM_PASSWORD", secret, sizeof(secret), err, sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "[SYS] DM password: %s\n", err);
        return false;
    }
    if (rc == 0) {
        server_log("SYS", "DM account disabled (no DM password configured).");
        return true;
    }
    if (err[0]) {
        fprintf(stderr, "[SYS] WARNING: %s\n", err);
    }
    why = auth_secret_problem(secret);
    bool ok = (why == NULL) && auth_set_dm_secret(secret);
    auth_wipe(secret, sizeof(secret));
    if (!ok) {
        fprintf(stderr, "[SYS] DM password %s.\n", why ? why : "could not be hashed");
        return false;
    }
    server_log("SYS", "DM account enabled.");
    return true;
}

static uint64_t g_login_gen;

/*MSG_LOGIN, first half (game loop): the cheap checks, then the Argon2id
 * work and the save read go to the login worker; login_finish() completes
 * the login. Every refusal goes through reject_login().*/
static void login_begin(Client *clients, int i, MsgLogin *ml) {
    /*wire_get_login() already guarantees NUL-terminated strings that fit
   * their fields.*/

    //Server access ("gate") password: keyed digest, constant-time compare.
    bool gate_ok = auth_check_gate(ml->server_pass);
    auth_wipe(ml->server_pass, sizeof(ml->server_pass));
    if (!gate_ok) {
        server_log("AUTH", "Server access denied (wrong server password) from %s", conn_peer_ip(i));
        reject_login(&clients[i], i, "Wrong server password.", true);
        return;
    }

    //--- Username policy (§1.3): [A-Za-z0-9_]{1,16} ---
    //The username goes into file names (saves/<u>.save,
    //saves/tombstone_<u>_*.dat): slashes, ".." etc. were path-
    //traversal vectors, so reject anything that doesn't match.
    if (!valid_username(ml->username)) {
        reject_login(&clients[i], i, "Invalid username: use 1-16 chars (a-z, A-Z, 0-9, _).", false);
        return;
    }

    //One session per character: two sessions would load and save
    //the same character independently (the last save wins), which
    //duplicates items. The live session keeps the character.
    if (character_online(clients, ml->username)) {
        reject_login(&clients[i], i, "Character already online.", false);
        return;
    }

    if (!auth_password_acceptable(ml->password)) {
        reject_login(&clients[i], i, "Invalid personal password: 1-63 printable characters.",
                     false);
        return;
    }

    //--- Dungeon Master: a separate credential. The account exists only if
    //the server was started with a DM password (checked by the worker). ---
    bool is_dm = (strcmp(ml->username, "dm") == 0);
    if (is_dm && !auth_dm_enabled()) {
        reject_login(&clients[i], i, "The DM account is disabled on this server.", false);
        return;
    }

    LoginJob *job = calloc(1, sizeof(*job));
    if (!job ||
        !state_path(job->save_path, sizeof(job->save_path), "saves/%s.save", ml->username)) {
        free(job);
        reject_login(&clients[i], i, "Server error while logging in.", false);
        return;
    }
    job->slot = i;
    job->gen = ++g_login_gen;
    job->is_dm = is_dm;
    job->ml = *ml; /*the gate password is already wiped*/
    clients[i].login_pending = true;
    clients[i].login_gen = job->gen;
    copy_str(clients[i].login_user, ml->username, sizeof(clients[i].login_user));
    login_worker_submit(job);
}

/*Ability scores, rolled on the server: 3d6 per ability + the race and
 * subrace bonuses, clamped to the playable range 3..18.*/
static void roll_abilities(Client *c) {
    const RaceType rt = c->race_id;
    int bonus[6] = {RACES[rt].str_bonus, RACES[rt].dex_bonus, RACES[rt].con_bonus,
                    RACES[rt].int_bonus, RACES[rt].wis_bonus, RACES[rt].cha_bonus};
    if (c->subrace_id >= 0) {
        const SubraceType st = c->subrace_id;
        bonus[0] += SUBRACES[st].str_bonus;
        bonus[1] += SUBRACES[st].dex_bonus;
        bonus[2] += SUBRACES[st].con_bonus;
        bonus[3] += SUBRACES[st].int_bonus;
        bonus[4] += SUBRACES[st].wis_bonus;
        bonus[5] += SUBRACES[st].cha_bonus;
    }
    c->str = clamp_stat(rules_roll_dice(3, 6) + bonus[0]);
    c->dex = clamp_stat(rules_roll_dice(3, 6) + bonus[1]);
    c->con = clamp_stat(rules_roll_dice(3, 6) + bonus[2]);
    c->intel = clamp_stat(rules_roll_dice(3, 6) + bonus[3]);
    c->wis = clamp_stat(rules_roll_dice(3, 6) + bonus[4]);
    c->cha = clamp_stat(rules_roll_dice(3, 6) + bonus[5]);
}

/*Slots of a new character: martial classes have level-1 slots only (for
 * their maneuvers), casters every level.*/
static void starting_spell_slots(Client *c) {
    memset(c->spell_slots_max, 0, sizeof(c->spell_slots_max));
    switch (c->class_id) {
    case CLASS_PALADIN:
    case CLASS_RANGER:
        c->spell_slots_max[1] = 21;
        break;
    case CLASS_BARBARIAN:
    case CLASS_ROGUE:
        c->spell_slots_max[1] = 22;
        break;
    case CLASS_FIGHTER:
    case CLASS_MONK:
        c->spell_slots_max[1] = 23;
        break;
    default:
        for (int s = 1; s <= 9; s++) {
            c->spell_slots_max[s] = 25 - s;
        }
        break;
    }
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
        c->spell_slots[s] = c->spell_slots_max[s];
    }
}

/*A new character in the city square. Race, class and alignment come from
 * the client and are range-checked (they index the RACES, SUBRACES,
 * CLASSES and ALIGNMENTS tables); the abilities are rolled here.*/
static void new_character(Client *c, const MsgLogin *ml) {
    c->floor_id = 0;
    c->max_floor_explored = 0;
    c->x = MAP_CENTER_X;
    c->y = MAP_CENTER_Y;
    c->gold = 1000;
    c->level = 1;
    c->xp = 0;
    c->backpack_count = 0;
    ItemInstance *slots[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                             &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                             &c->slot_arm_l,  &c->slot_feet};
    for (size_t k = 0; k < sizeof(slots) / sizeof(slots[0]); k++) {
        slots[k]->template_idx = -1;
    }
    for (int r = 0; r < 10; r++) {
        c->slot_rings[r].template_idx = -1;
    }
    for (int b = 0; b < MAX_BELT; b++) {
        c->belt[b].template_idx = -1;
    }
    c->pending_trade_merchant_id = -1;
    c->race_id = (ml->race_id >= 0 && ml->race_id < RACE_COUNT) ? (RaceType)ml->race_id : 0;
    c->subrace_id =
        (ml->subrace_id >= -1 && ml->subrace_id < SUBRACE_COUNT) ? (SubraceType)ml->subrace_id : -1;
    c->class_id = (ml->class_id >= 0 && ml->class_id < CLASS_COUNT) ? (ClassType)ml->class_id
                                                                    : CLASS_BARBARIAN;
    c->alignment = (ml->alignment >= 0 && ml->alignment < ALIGN_COUNT) ? ml->alignment : 0;
    roll_abilities(c);
    c->hp = 20 + rules_get_modifier(c->con);
    c->max_hp = c->hp;
    starting_spell_slots(c);
    give_starting_gear(c);
}

/*The character enters the world: welcome, state, map, the entities
 * around, its place on the grid. False if the client is already gone.*/
static bool enter_world(Client *c, Game *game, bool loaded) {
    MsgWelcome mw;
    memset(&mw, 0, sizeof(mw));
    mw.entity_id = c->entity_id;
    mw.x = c->x;
    mw.y = c->y;
    mw.is_new = loaded ? 0 : 1;
    mw.hp = c->hp;
    mw.max_hp = c->max_hp;
    mw.gold = c->gold;
    mw.race_id = (int)c->race_id;
    mw.subrace_id = (int)c->subrace_id;
    mw.class_id = (int)c->class_id;
    mw.level = c->level;
    mw.alignment = c->alignment;
    if (!send_welcome(c->sock, &mw)) {
        return false;
    }
    send_detailed_state(game, c);
    send_map_chunk(c->sock, &game->world->floors[c->floor_id].map, c->x, c->y, INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, game);
    broadcast_player_state(game, c);
    game->world->floors[c->floor_id].entity_grid[c->y][c->x] = c->entity_id;
    if (loaded) {
        send_text_to_client(c->sock, "[SYSTEM] Welcome back, %s! (Floor %d, HP %d/%d, Gold %lu gp)",
                            c->username, c->floor_id, c->hp, c->max_hp, (unsigned long)c->gold);
    } else {
        send_text_to_client(c->sock, "[SYSTEM] Welcome, %s! Your adventure begins...", c->username);
        send_text_to_client(c->sock,
                            "[CHARACTER] Rolled by the server: STR %d, DEX %d, CON %d, "
                            "INT %d, WIS %d, CHA %d (Max HP %d)",
                            c->str, c->dex, c->con, c->intel, c->wis, c->cha, c->max_hp);
    }
    return true;
}

/*What the client is told when the worker refuses the login; NULL when it
 * accepts. *count is set when the refusal counts against the address.*/
static const char *login_refusal(LoginOutcome outcome, bool *count) {
    *count = outcome == LOGIN_BAD_DM || outcome == LOGIN_BAD_PASSWORD;
    switch (outcome) {
    case LOGIN_BAD_DM:
        return "Invalid DM password.";
    case LOGIN_BAD_PASSWORD:
        return "Incorrect character password.";
    case LOGIN_UNREADABLE: /*other format version or CRC mismatch: the file is kept*/
        return "Save file unreadable (other format version or corrupt).";
    case LOGIN_SERVER_ERROR:
        return "Server error while creating the character.";
    case LOGIN_LOADED:
    case LOGIN_NEW:
        break;
    }
    return NULL;
}

/*MSG_LOGIN, second half (game loop): the worker's verdict. The slot may
 * have been dropped (timeout, disconnect) and even reused meanwhile: the
 * result only applies to the very connection that asked for it.*/
void login_finish(Client *clients, LoginJob *job, Game *game) {
    int i = job->slot;
    if (i < 0 || i >= MAX_CLIENTS || !clients[i].active || !clients[i].login_pending ||
        clients[i].login_gen != job->gen || conn_dropping(i)) {
        return;
    }
    Client *c = &clients[i];
    c->login_pending = false;
    c->login_user[0] = '\0';
    log_login_outcome(job);
    bool count_failure = false;
    const char *refusal = login_refusal(job->outcome, &count_failure);
    if (refusal) {
        reject_login(c, i, refusal, count_failure);
        return;
    }
    bool loaded = job->outcome == LOGIN_LOADED;
    c->entity_id = game->next_id++;
    copy_str(c->username, job->ml.username, sizeof(c->username));
    c->effect_count = 0;
    /*The saved character (its Argon2id hash verified by the worker), or a
     * new one with the hash the worker computed.*/
    if (loaded) {
        apply_player_save(c, &job->sd);
    } else {
        copy_str(c->password_hash, job->new_hash, sizeof(c->password_hash));
    }
    c->authenticated = true;
    c->is_dm = job->is_dm;
    if (c->is_dm) {
        server_log("AUTH", "DM account authenticated from %s.", conn_peer_ip(i));
    }
    if (!loaded) {
        new_character(c, &job->ml);
    }
    /*A failed welcome means the client is gone (the send already dropped
     * it): the rest of the login is skipped.*/
    if (!enter_world(c, game, loaded)) {
        return;
    }
    ipguard_login_succeeded(conn_peer_ip(i));
    server_log("AUTH", "'%s' %s", job->ml.username, loaded ? "logged in" : "created");
}

void handle_login(Client *clients, int i, const void *payload, uint32_t len) {
    if (clients[i].authenticated) {
        conn_mark_drop(i, "MSG_LOGIN on an authenticated session");
        return;
    }
    if (clients[i].login_pending) {
        conn_mark_drop(i, "second MSG_LOGIN while the first is being checked");
        return;
    }
    MsgLogin ml;
    if (!wire_get_login(payload, len, &ml)) {
        auth_wipe(&ml, sizeof(ml));
        conn_wipe_message(i);
        conn_mark_drop(i, "malformed MSG_LOGIN");
        return;
    }
    login_begin(clients, i, &ml);
    auth_wipe(&ml, sizeof(ml)); /*the job keeps the only copy, wiped by the worker*/
    conn_wipe_message(i);
}
