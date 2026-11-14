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

#include "game.h"
#include "wire.h"
#include "server_conn.h"
#include "server_options.h"
#include "server_tls.h"
#include "writer.h"
#include <errno.h>
#include <sys/epoll.h>
#include <signal.h>

#include "aoe.h"
#include "server_internal.h"

/*Keep the protocol constant in sync with the server enum*/
_Static_assert(BOOKS_MARTIAL == SHOP_SPEC_BOOKS_MARTIAL,
               "SHOP_SPEC_BOOKS_MARTIAL must equal MerchantSpecialization::BOOKS_MARTIAL");
#include "server_commands.h"
#include "server_move.h"
#include "spell_router.h"
#include "player.h"
#include "server_log.h"
#include "server_login.h"
#include "server_metrics.h"
#include "server_loot.h"
#include "server_startup.h"
#include "server_tiles.h"

/*--- Graceful shutdown (Ctrl-C / SIGTERM) ---
* The handler only sets a flag (async-signal-safe); the main loop
* notices it on the next tick and exits through the normal cleanup
* path, so clog_close() can write the closing bracket of
* combat_log.json.*/
static volatile sig_atomic_t g_shutdown_requested = 0;

/*epoll data.u32 of the descriptors that are not client slots (a client is
 * identified by its slot index, 0..MAX_CLIENTS-1).*/
#define EPOLL_ID_LISTEN ((uint32_t)MAX_CLIENTS)
#define EPOLL_ID_LOGIN  ((uint32_t)MAX_CLIENTS + 1)
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static void flush_all_clients(Client *clients) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && conn_tx_pending(i)) {
            conn_flush(i);
        }
    }
}

/*Remove every connection marked for removal during this iteration. This
 * is the ONLY place a client slot is closed: saving, the "left floor"
 * notice and the grid cleanup happen once, in one order, and no send path
 * ever disconnects a client synchronously.*/
static void reap_dropped_clients(Game *game, Client *clients) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active || !conn_dropping(i)) {
            continue;
        }
        Client *c = &clients[i];
        server_log("NET", "Client %d ('%s', %s) disconnected: %s", i,
                   c->authenticated ? c->username : "-", conn_peer_ip(i), conn_drop_reason(i));
        if (c->authenticated) {
            save_player_data(c);
            if (c->floor_id >= 0 && c->floor_id < MAX_FLOORS && c->x >= 0 && c->x < MAP_WIDTH &&
                c->y >= 0 && c->y < MAP_HEIGHT) {
                int *cell = &game->world->floors[c->floor_id].entity_grid[c->y][c->x];
                if (*cell == c->entity_id) {
                    *cell = 0;
                }
                notify_player_left_floor(game, c, c->floor_id);
            }
        }
        conn_close(i); /*a queued MSG_AUTH_FAIL first, then close_notify*/
        c->sock = -1;
        c->active = false;
        c->authenticated = false;
        c->is_dm = false;
    }
}

/*Interest set of every slot: EPOLLOUT only while bytes are waiting
 * (level-triggered EPOLLOUT on an empty queue would wake the loop
 * continuously), and no EPOLLIN while the slot's login is in the worker
 * (its unread bytes would wake the loop continuously too).*/
static void update_interest(int epoll_fd, Client *clients) {
    static uint32_t current[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            current[i] = 0;
            continue;
        }
        uint32_t want = EPOLLRDHUP | (clients[i].login_pending ? 0u : EPOLLIN) |
                        (conn_tx_pending(i) ? EPOLLOUT : 0u);
        if (current[i] == 0) {
            current[i] = EPOLLIN | EPOLLRDHUP; /*as registered by accept*/
        }
        if (want == current[i]) {
            continue;
        }
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = want;
        ev.data.u32 = (uint32_t)i;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, clients[i].sock, &ev) == 0) {
            current[i] = want;
        }
    }
}

/*SIGINT/SIGTERM request a graceful shutdown (combat_log.json would be
 * left truncated otherwise); a write to a closed socket fails with EPIPE
 * instead of killing the process.*/
static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/*Text commands have their own budget, apart from the steps (a command
 * typed while walking must not be lost): CMD_PER_SECOND in any second. A
 * client over it is told once per second, and the command is dropped.*/
#define CMD_PER_SECOND 10

static bool command_allowed(Client *c, long long now_ms) {
    if (now_ms - c->cmd_window_ms >= 1000) {
        c->cmd_window_ms = now_ms;
        c->cmd_in_window = 0;
    }
    if (c->cmd_in_window < CMD_PER_SECOND) {
        c->cmd_in_window++;
        return true;
    }
    if (c->cmd_in_window == CMD_PER_SECOND) {
        c->cmd_in_window++; /*one notice per window*/
        send_text_to_client(c->sock, "[SYSTEM] Too many commands: wait a moment.");
    }
    return false;
}

static void handle_text_message(Client *clients, int i, const void *payload, uint32_t len,
                                Game *game) {
    Client *c = &clients[i];
    MsgTextCmd tc;
    if (!wire_get_text_cmd(payload, len, &tc)) {
        conn_mark_drop(i, "malformed MSG_TEXT_CMD");
        return;
    }
    if (!command_allowed(c, get_time_ms())) {
        return;
    }
    /*No control characters reach the game: they would travel verbatim to
   * other players' terminals (ANSI escapes, \r overwriting lines) through
   * chat and private messages.*/
    for (char *p = tc.cmd; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7F) {
            *p = ' ';
        }
    }
    handle_text_cmd(c, tc.cmd, game);
}

/*One complete message of slot i. Anything but MSG_LOGIN needs a login.*/
static void handle_message(Client *clients, int i, const MsgHeader *hdr, const void *payload,
                           Game *game) {
    if (hdr->type == MSG_LOGIN) {
        handle_login(clients, i, payload, hdr->length);
    } else if (hdr->type == MSG_MOVE) {
        if (!clients[i].authenticated) {
            conn_mark_drop(i, "MSG_MOVE before login");
            return;
        }
        MsgMove m;
        if (!wire_get_move(payload, hdr->length, &m)) {
            conn_mark_drop(i, "malformed MSG_MOVE");
            return;
        }
        player_move(&clients[i], &m, clients, game);
    } else if (hdr->type == MSG_TEXT_CMD) {
        if (!clients[i].authenticated) {
            conn_mark_drop(i, "MSG_TEXT_CMD before login");
            return;
        }
        handle_text_message(clients, i, payload, hdr->length, game);
    }
}

/*Socket event of slot i: drain the socket, then handle every COMPLETE
 * message already buffered. Nothing waits for missing bytes: a partial
 * message stays in the buffer until the next EPOLLIN (server_conn.h).*/
static void serve_client(Client *clients, int i, uint32_t ev_mask, Game *game) {
    if (!clients[i].active || conn_dropping(i)) {
        return;
    }
    /*While its login is being checked a slot reads nothing (its EPOLLIN
   * interest is off, see update_interest): only a hang-up is acted upon.*/
    if (clients[i].login_pending) {
        if (ev_mask & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
            conn_mark_drop(i, "connection closed during login");
        }
        return;
    }
    ConnRxStatus rx;
    MsgHeader hdr;
    const void *payload = NULL;
    int got = 0;
    /*A MSG_LOGIN hands the slot to the login worker: the messages after it
   * stay buffered until login_finish() (resume list). Bytes TLS decrypted
   * beyond the receive buffer are read once messages made room.*/
    do {
        rx = conn_read(i);
        while (clients[i].active && !conn_dropping(i) && !clients[i].login_pending &&
               (got = conn_next_message(i, &hdr, &payload)) == 1) {
            handle_message(clients, i, &hdr, payload, game);
        }
    } while (got == 0 && rx == CONN_RX_OK && clients[i].active && !conn_dropping(i) &&
             !clients[i].login_pending && conn_input_buffered(i));
    if (got < 0) {
        server_log("NET", "Protocol violation from client %d (%s): disconnect", i, conn_peer_ip(i));
        conn_mark_drop(i, "protocol violation");
    } else if (rx == CONN_RX_CLOSED) {
        conn_mark_drop(i, "connection closed by peer");
    } else if (rx == CONN_RX_ERROR) {
        conn_mark_drop(i, "socket error");
    }
}

/*Completed logins. Slots that already have messages buffered are added to
 * resume[], to be served as if their socket had fired.*/
static void take_finished_logins(Client *clients, Game *game, int *resume, int *n_resume) {
    LoginJob *job;
    while ((job = login_worker_take()) != NULL) {
        int slot = job->slot;
        login_finish(clients, job, game);
        login_job_free(job);
        if (slot >= 0 && slot < MAX_CLIENTS && clients[slot].active &&
            clients[slot].authenticated && !conn_dropping(slot) && conn_rx_pending(slot) &&
            *n_resume < MAX_CLIENTS) {
            resume[(*n_resume)++] = slot;
        }
    }
}

/*The main loop, until SIGINT/SIGTERM.
 *
 * update_world() runs on a fixed-step clock: exactly 1/TICK_MS Hz (5
 * rounds/s) of real time, whatever the event loop does, so the game speed
 * never follows the CPU load. A pass runs at most MAX_TICK_CATCHUP_MS of
 * simulation; a stalled system drops the excess instead of fast-forwarding.
 * epoll waits half a tick, so input is served within half a round.*/
/*Budgets of the two measured durations (server_metrics.h): a round over
 * TICK_BUDGET_MS, or a pass of the loop over half a round, is counted and
 * reported. PERF_LOG_ROUNDS: one [PERF] line every 5 minutes.*/
#define TICK_BUDGET_MS  50
#define LOOP_BUDGET_MS  (TICK_MS / 2)
#define PERF_LOG_ROUNDS 1500

/*One [PERF] line for the window that just ended (or, at shutdown, for the
 * life of the process).*/
static void log_performance(bool lifetime) {
    MetricSummary tick, loop;
    metrics_summary(METRIC_TICK, lifetime, &tick);
    metrics_summary(METRIC_LOOP, lifetime, &loop);
    if (tick.count == 0) {
        return;
    }
    unsigned queued = 0, queued_max = 0;
    writer_queue_depth(&queued, &queued_max);
    server_log("PERF",
               "%s: %llu rounds, round p50 %.2f p99 %.2f max %.2f ms (%llu over %d ms); "
               "loop p99 %.2f max %.2f ms (%llu over %d ms); writer queue %u (max %u), "
               "logins waiting %u",
               lifetime ? "total" : "last 5 min", (unsigned long long)tick.count, tick.p50_ms,
               tick.p99_ms, tick.max_ms, (unsigned long long)tick.over_budget, TICK_BUDGET_MS,
               loop.p99_ms, loop.max_ms, (unsigned long long)loop.over_budget, (int)LOOP_BUDGET_MS,
               queued, queued_max, login_worker_backlog());
}

static void run_server(int s_sock, int epoll_fd, Client *clients, Game *game) {
    struct epoll_event ep_events[MAX_CLIENTS + 2 + CONN_REFUSAL_SLOTS];
    long long last_iter_ms = 0;
    long long tick_acc_ms = 0;
    while (!g_shutdown_requested) {
        int n_events =
            epoll_wait(epoll_fd, ep_events, (int)(sizeof(ep_events) / sizeof(ep_events[0])),
                       (int)(TICK_MS / 2));
        uint64_t pass_start_us = metrics_now_us();
        int resume[MAX_CLIENTS];
        int n_resume = 0;
        if (n_events < 0) {
            n_events = 0;
        }
        for (int ev_idx = 0; ev_idx < n_events + n_resume; ev_idx++) {
            uint32_t id = (ev_idx < n_events) ? ep_events[ev_idx].data.u32
                                              : (uint32_t)resume[ev_idx - n_events];
            uint32_t ev_mask = (ev_idx < n_events) ? ep_events[ev_idx].events : EPOLLIN;
            if (id == EPOLL_ID_LISTEN) {
                accept_pending_connections(s_sock, epoll_fd, clients);
            } else if (id == EPOLL_ID_LOGIN) {
                take_finished_logins(clients, game, resume, &n_resume);
            } else if (conn_is_refusal_id(id)) {
                conn_refusal_event(epoll_fd, id, get_time_ms());
            } else {
                serve_client(clients, (int)id, ev_mask, game);
            }
        }
        expire_pending_logins(clients);
        conn_refusals_expire(epoll_fd, get_time_ms());

        long long now_ms = get_time_ms();
        if (last_iter_ms == 0) {
            last_iter_ms = now_ms;
        }
        tick_acc_ms += now_ms - last_iter_ms;
        last_iter_ms = now_ms;
        if (tick_acc_ms > MAX_TICK_CATCHUP_MS) {
            tick_acc_ms = MAX_TICK_CATCHUP_MS; /*drop the excess, never fast-forward*/
        }
        while (tick_acc_ms >= TICK_MS) {
            uint64_t round_start_us = metrics_now_us();
            update_world(clients, game);
            metrics_record(METRIC_TICK, metrics_now_us() - round_start_us);
            tick_acc_ms -= TICK_MS;
            if (game->total_turns % PERF_LOG_ROUNDS == 0) {
                log_performance(false);
                metrics_window_reset();
            }
        }
        /*Write what the kernel accepts, remove the clients marked for removal
     * (their "left floor" notices are queued for the others), write again,
     * then ask epoll for EPOLLOUT only where bytes are still waiting.*/
        flush_all_clients(clients);
        reap_dropped_clients(game, clients);
        flush_all_clients(clients);
        update_interest(epoll_fd, clients);
        pdf_jobs_poll(game);
        metrics_record(METRIC_LOOP, metrics_now_us() - pass_start_us);
    }
}

/*Graceful shutdown: logins still being checked are abandoned, queued
 * writes reach the disk, then every character, the world and the artifacts
 * are written synchronously (the autosave runs once a minute).*/
static void shutdown_server(Client *clients, Game *game) {
    log_performance(true);
    login_worker_stop();
    if (!writer_stop()) {
        server_log("SYS", "WARNING: some queued saves failed (see above).");
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated) {
            save_player_data(&clients[i]);
        }
    }
    if (save_world_state(game)) {
        server_log("SYS", "World and NPCs saved.");
    }
    artifacts_save();
    clog_close();
    server_tls_cleanup();
    server_log("SYS", "Server shut down. Log saved.");
}

/*Listening socket and epoll set, with the listener registered. False if
 * either cannot be created.*/
static bool open_listener(int port, int *s_sock, int *epoll_fd) {
    *s_sock = net_create_server(port);
    if (*s_sock < 0) {
        server_log("SYS", "Cannot bind to port %d", port);
        return false;
    }
    server_log("SYS", "Listening on port %d", port);
    net_set_nonblocking(*s_sock);
    *epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (*epoll_fd == -1) {
        server_log("SYS", "Cannot create epoll");
        return false;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = EPOLL_ID_LISTEN;
    epoll_ctl(*epoll_fd, EPOLL_CTL_ADD, *s_sock, &ev);
    return true;
}

/*Saves go through the writer thread and logins through the login worker
 * from here on: neither touches the game state.*/
static bool start_workers(int epoll_fd) {
    writer_start();
    if (!login_worker_start()) {
        server_log("SYS", "Cannot start the login worker: %s", strerror(errno));
        return false;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = EPOLL_ID_LOGIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, login_worker_eventfd(), &ev) != 0) {
        server_log("SYS", "epoll_ctl(login worker) failed: %s", strerror(errno));
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    static Game s_game;
    Game *game = &s_game;
    setvbuf(stdout, NULL, _IONBF, 0); /*live log files*/
    if (argc == 2 && strcmp(argv[1], "--print-commands") == 0) {
        cmd_help_markdown(stdout); /*docs/COMMANDS.md*/
        return 0;
    }
    ServerOptions opt = {.port = 8080};
    int status = parse_options(argc, argv, &opt);
    if (status >= 0) {
        return status;
    }
    if (!auth_init()) {
        fprintf(stderr, "%s: libsodium initialization failed\n", argv[0]);
        return 1;
    }
    if (!game_create(game)) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }
    if (!load_server_secrets(opt.password_file, opt.dm_password_file)) {
        return 2;
    }
    {
        char err[STATE_DIR_MAX + 128];
        if (!persist_init_state_dir(opt.state_dir, err, sizeof(err))) {
            fprintf(stderr, "[SYS] State directory: %s\n", err);
            return 2;
        }
    }
    {
        char err[STATE_DIR_MAX + 256], warn[STATE_DIR_MAX + 64];
        if (!server_tls_init(opt.tls_cert, opt.tls_key, err, sizeof(err), warn, sizeof(warn))) {
            fprintf(stderr, "[SYS] TLS: %s\n", err);
            return 2;
        }
        if (warn[0]) {
            fprintf(stderr, "[SYS] WARNING: %s\n", warn);
        }
    }
    if (opt.data_dir) {
        copy_str(game->data_dir, opt.data_dir, sizeof(game->data_dir));
    }
    resolve_data_dir(game);
    /*Before anything else: even a world loaded from disk must not roll its
   * dice on the default seed.*/
    seed_game_rng();
    server_log("SYS", "Starting Dragon GL Server...");
    server_log("SYS", "Data directory (read-only): %s", game->data_dir);
    server_log("SYS", "State directory: %s", persist_state_dir());
    server_log("SYS", "TLS certificate: %s", server_tls_cert_path());
    server_log("SYS", "TLS fingerprint: %s", server_tls_fingerprint());
    {
        char clog_path[STATE_DIR_MAX + 32];
        if (state_path(clog_path, sizeof(clog_path), "combat_log.json")) {
            clog_init(clog_path);
            server_log("SYS", "Tactical log opened: %s", clog_path);
        }
    }
    install_signal_handlers();
    int s_sock = -1, epoll_fd = -1;
    if (!open_listener(opt.port, &s_sock, &epoll_fd)) {
        clog_close();
        return 1;
    }

    Client *clients = game->clients;
    status = load_game_data(game);
    if (status != 0) {
        clog_close();
        return status;
    }
    artifacts_load();
    server_log("SYS", "Unique Artifact System initialized.");
    persist_cleanup_tmp(); /*leftovers of a write interrupted by a crash*/
    tombstone_load_all(game);
    server_log("SYS", "Tombstone System initialized.");

    if (!load_or_generate_world(game)) {
        clog_close();
        return 1;
    }
    sync_entity_grid(game);
    floor_stats_rebuild(game);
    floor_index_rebuild(game);
    aoe_init_clouds(game);
    spell_router_init();
    metrics_init((uint64_t)TICK_BUDGET_MS * 1000u, (uint64_t)LOOP_BUDGET_MS * 1000u);
    if (!start_workers(epoll_fd)) {
        clog_close();
        return 1;
    }
    server_log("SYS", "Server ready!");
    run_server(s_sock, epoll_fd, clients, game);
    shutdown_server(clients, game);
    return 0;
}
