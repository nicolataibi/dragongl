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
 * cmd_dm.c - Dungeon Master commands (dm_*), only for the dm account, and the
 * background PDF map generator they start.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "strnum.h"
#include "server_cmd.h"
#include "dragongl_config.h"
#include "persist.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "ai.h"
#include "server_world.h"
#include "player.h"
#include "server_log.h"
#include "server_tiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

/*POSIX: the process environment passed to posix_spawn*/
extern char **environ;

/* Dumps one floor to a text file understood by tools/generate_pdf_map.py.
 * Format v2: a small header (floor, size, optional player position) followed
 * by one line per row where each tile is its 2-digit hex VoxelType
 * (00 = VOXEL_ROCK ... 1B = VOXEL_CRYSTAL_WHITE, see include/map.h), so the
 * PDF can render every tile with its exact in-game color. */
static int dump_floor_v2(Game *game, const char *path, int floor_id, const Client *c) {
    FILE *fp = fopen(path, "we");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "# DragonGL map dump v2\n");
    fprintf(fp, "floor %d\n", floor_id);
    fprintf(fp, "width %d\n", MAP_WIDTH);
    fprintf(fp, "height %d\n", MAP_HEIGHT);
    if (c && floor_id == c->floor_id && c->x >= 0 && c->x < MAP_WIDTH && c->y >= 0 &&
        c->y < MAP_HEIGHT) {
        fprintf(fp, "player %d %d\n", c->x, c->y);
    }
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            fprintf(fp, "%02x", (int)game->world->floors[floor_id].map.data[0][y][x]);
        }
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

/* Resolves "python3" to an ABSOLUTE path for posix_spawn.
 *
 * glibc's posix_spawn() does NOT search PATH when the file contains no
 * slash (POSIX leaves the search behavior implementation-defined): a bare
 * posix_spawn("python3", ...) fails with ENOENT even when python3 is
 * perfectly reachable, so every dm_mapfloor/dm_pdf died with "No such
 * file or directory". Search PATH ourselves (execvp semantics) and, when
 * the server runs without one (service managers, env -i), fall back to
 * the standard absolute locations. */
static int resolve_python3(char *out, size_t out_len) {
    const char *path = getenv("PATH");
    if (path && *path) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", path);
        char *save = NULL;
        for (char *dir = strtok_r(buf, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
            if (dir[0] == '\0') {
                continue;
            }
            char cand[4096];
            size_t dl = strlen(dir);
            snprintf(cand, sizeof(cand), "%s%spython3", dir,
                     (dl > 0 && dir[dl - 1] == '/') ? "" : "/");
            if (access(cand, X_OK) == 0) {
                snprintf(out, out_len, "%s", cand);
                return 0;
            }
        }
    }
    static const char *const fallbacks[] = {"/usr/bin/python3", "/usr/local/bin/python3",
                                            "/bin/python3"};
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (access(fallbacks[i], X_OK) == 0) {
            snprintf(out, out_len, "%s", fallbacks[i]);
            return 0;
        }
    }
    return 1;
}

/* Locates tools/generate_pdf_map.py next to the server binary (build tree
 * or source tree layout) or in the installed DRAGONGL_PKGDATADIR/tools. The current
 * directory is never searched: a server started from an arbitrary (or
 * attacker-writable) directory must not run a script found there. */
static int resolve_pdf_script(char *out, size_t out_len) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash != NULL) {
            *slash = '\0'; /* directory of the binary */
            char cand[sizeof(exe) + 64];
            snprintf(cand, sizeof(cand), "%s/tools/generate_pdf_map.py", exe);
            if (access(cand, R_OK) == 0 && strlen(cand) < out_len) {
                copy_str(out, cand, out_len);
                return 0;
            }
            /* build/ layout: the binary sits in <root>/build, tools/ in <root> */
            snprintf(cand, sizeof(cand), "%s/../tools/generate_pdf_map.py", exe);
            if (access(cand, R_OK) == 0 && strlen(cand) < out_len) {
                copy_str(out, cand, out_len);
                return 0;
            }
        }
    }
    const char *installed = DRAGONGL_PKGDATADIR "/tools/generate_pdf_map.py";
    if (access(installed, R_OK) == 0) {
        snprintf(out, out_len, "%s", installed);
        return 0;
    }
    return 1;
}

/*--- DM PDF maps ------------------------------------------------------
 * tools/generate_pdf_map.py turns a floor dump into a color PDF. It runs
 * for seconds, so it is spawned in the background: pdf_jobs_poll() (once
 * per main-loop pass) reaps it with waitpid(WNOHANG) and tells the DM —
 * if the DM is still connected — how it went.
 *
 * posix_spawn instead of system(): a clean argument vector, no shell.
 * Both the interpreter and the script are resolved to real paths first
 * (resolve_python3 / resolve_pdf_script): glibc's posix_spawn performs no
 * PATH search. Every descriptor of the server is close-on-exec, so the
 * child inherits neither sockets nor the state-directory lock.*/
#define PDF_JOBS_MAX 2

typedef struct {
    pid_t pid; /*0 = free*/
    int floor_id;
    char dm_user[32];
    char pdf[STATE_DIR_MAX + 64];
} PdfJob;

static PdfJob g_pdf_jobs[PDF_JOBS_MAX];

static void pdf_notify(Game *game, const PdfJob *j, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client *c = &game->clients[i];
        if (c->active && c->authenticated && c->is_dm && strcmp(c->username, j->dm_user) == 0) {
            send_text_to_client(c->sock, "%s", msg);
            return;
        }
    }
}

void pdf_jobs_poll(Game *game) {
    for (int k = 0; k < PDF_JOBS_MAX; k++) {
        PdfJob *j = &g_pdf_jobs[k];
        if (j->pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t r = waitpid(j->pid, &status, WNOHANG);
        if (r == 0 || (r < 0 && errno == EINTR)) {
            continue; /*still running*/
        }
        char msg[STATE_DIR_MAX + 160];
        if (r < 0) {
            snprintf(msg, sizeof(msg),
                     "[DM] Error generating PDF: lost track of the generator (%s).",
                     strerror(errno));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            snprintf(msg, sizeof(msg), "[DM] Generated PDF Map: %s (Floor %d)", j->pdf,
                     j->floor_id);
        } else if (WIFEXITED(status)) {
            snprintf(
                msg, sizeof(msg),
                "[DM] Error generating PDF: generator exited with code %d (see server console for details).",
                WEXITSTATUS(status));
        } else {
            snprintf(msg, sizeof(msg), "[DM] Error generating PDF: generator killed by signal %d.",
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        server_log("DM", "%s", msg);
        pdf_notify(game, j, msg);
        memset(j, 0, sizeof(*j));
    }
}

/*dm_pdf <floor> / dm_mapfloor: dump the floor, start the generator.*/
static void dm_pdf_map(Game *game, Client *c, int floor_id) {
    PdfJob *slot = NULL;
    for (int k = 0; k < PDF_JOBS_MAX; k++) {
        if (g_pdf_jobs[k].pid > 0 && g_pdf_jobs[k].floor_id == floor_id) {
            /*Its dump file is being read: do not overwrite it.*/
            send_text_to_client(c->sock, "[DM] The PDF of floor %d is already being generated.",
                                floor_id);
            return;
        }
        if (g_pdf_jobs[k].pid <= 0 && !slot) {
            slot = &g_pdf_jobs[k];
        }
    }
    if (!slot) {
        send_text_to_client(c->sock,
                            "[DM] %d PDF maps are already being generated: try again shortly.",
                            PDF_JOBS_MAX);
        return;
    }
    char dump[STATE_DIR_MAX + 64], pdf[STATE_DIR_MAX + 64];
    if (!state_path(dump, sizeof(dump), "maps/map_dump_floor_%d.txt", floor_id) ||
        !state_path(pdf, sizeof(pdf), "maps/map_floor_%d.pdf", floor_id) ||
        dump_floor_v2(game, dump, floor_id, c) != 0) {
        send_text_to_client(c->sock, "[DM] Error writing the map dump in the state directory.");
        return;
    }
    char python[4096], script[4096];
    if (resolve_python3(python, sizeof(python)) != 0) {
        send_text_to_client(c->sock,
                            "[DM] Error: python3 not found (PATH searched, then /usr/bin, "
                            "/usr/local/bin and /bin): install Python 3 or fix the server PATH.");
        return;
    }
    if (resolve_pdf_script(script, sizeof(script)) != 0) {
        send_text_to_client(c->sock,
                            "[DM] Error: tools/generate_pdf_map.py not found (searched next to the "
                            "server binary and in " DRAGONGL_PKGDATADIR "/tools).");
        return;
    }
    char floor_arg[16];
    snprintf(floor_arg, sizeof(floor_arg), "%d", floor_id);
    char *argv[] = {python, script, dump, pdf, floor_arg, NULL};
    pid_t pid;
    int spawn_err = posix_spawn(&pid, python, NULL, NULL, argv, environ);
    if (spawn_err != 0) {
        send_text_to_client(c->sock, "[DM] Error: failed to launch PDF generator (%s).",
                            strerror(spawn_err));
        return;
    }
    slot->pid = pid;
    slot->floor_id = floor_id;
    copy_str(slot->dm_user, c->username, sizeof(slot->dm_user));
    copy_str(slot->pdf, pdf, sizeof(slot->pdf));
    send_text_to_client(c->sock, "[DM] Generating the PDF of floor %d in the background...",
                        floor_id);
}

static bool cmd_dm_gold(Client *c, const char *cmd, Game *game) {
    (void)game;
    /*A bounded amount: the negation below must not overflow.*/
    long long amount = 0;
    if (!parse_ll(cmd + 8, &amount, NULL) || amount < -1000000000000LL ||
        amount > 1000000000000LL) {
        send_text_to_client(c->sock, "[DM] Use: dm_gold <amount, -10^12..10^12>");
        return true;
    }
    if (amount < 0 && c->gold < (uint64_t)(-amount)) {
        c->gold = 0;
    } else if (amount < 0) {
        c->gold -= (uint64_t)(-amount);
    } else {
        c->gold += (uint64_t)amount;
    }
    send_text_to_client(c->sock, "[DM] %lld's updated gold. Total: %llu", amount,
                        (unsigned long long)c->gold);
    return true;
}

static bool cmd_dm_level(Client *c, const char *cmd, Game *game) {
    (void)game;
    int target = str_to_int(cmd + 9);
    if (c->level >= 20) {
        send_text_to_client(c->sock, "[DM] You are already at maximum level (20).");
        return true;
    }
    if (target <= c->level || target > 20) {
        send_text_to_client(c->sock, "[DM] Use: dm_level <target> (must be between %d and 20)",
                            c->level + 1);
        return true;
    }
    c->xp = get_xp_threshold(target);
    check_level_up(c);
    send_text_to_client(c->sock, "[DM] Level increased to required target.");
    return true;
}

static bool cmd_dm_find_monster(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *search = cmd + 16;
    int found_count = 0;
    send_text_to_client(c->sock, "[DM] Monster '%s' search results:", search);
    for (int i = 0; i < bestiary_size; i++) {
        if (strcasestr(bestiary_data[i].name, search)) {
            send_text_to_client(c->sock, "  ID: %d - %s", i, bestiary_data[i].name);
            found_count++;
            if (found_count >= 10) {
                send_text_to_client(c->sock, "...only the first 10 results shown.");
                break;
            }
        }
    }
    if (found_count == 0) {
        send_text_to_client(c->sock, "No monsters found.");
    }
    return true;
}

static bool cmd_dm_find_item(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *search = cmd + 13;
    int found_count = 0;
    send_text_to_client(c->sock, "[DM] Object '%s' search results:", search);
    for (int i = 0; i < item_database_size; i++) {
        if (strcasestr(item_database[i].name, search)) {
            send_text_to_client(c->sock, "  ID: %d - %s", i, item_database[i].name);
            found_count++;
            if (found_count >= 10) {
                send_text_to_client(c->sock, "...only the first 10 results shown.");
                break;
            }
        }
    }
    if (found_count == 0) {
        send_text_to_client(c->sock, "No items found.");
    }
    return true;
}

static bool cmd_dm_telemetry(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    int active_c = 0, active_n = 0, total_t = 0, active_t = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (game->clients[i].active) {
            active_c++;
        }
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        if (game->npcs[i].active) {
            active_n++;
        }
    }
    for (int f = 0; f < MAX_FLOORS; f++) {
        total_t += game->world->floors[f].trap_count;
        for (int j = 0; j < game->world->floors[f].trap_count; j++) {
            if (game->world->floors[f].traps[j].active) {
                active_t++;
            }
        }
    }
    send_text_to_client(c->sock, "[DM] Telemetry: Turns:%d, Clients:%d, NPCs:%d/%d, Traps:%d/%d",
                        game->total_turns, active_c, active_n, MAX_NPCS, active_t, total_t);
    return true;
}

static bool cmd_dm_spawn_boss(Client *c, const char *cmd, Game *game) {
    int target_floor = 0;
    /*%d skips the leading space after the command word*/
    if (!parse_int(cmd + 13, &target_floor, NULL) || target_floor <= 0 ||
        target_floor >= MAX_FLOORS) {
        send_text_to_client(c->sock, "[DM] Use: dm_spawn_boss <floor 1-%d> (ex: dm_spawn_boss 10)",
                            MAX_FLOORS - 1);
        return true;
    }

    NPC *b = npc_alloc(game);
    if (!b) {
        send_text_to_client(c->sock, "[DM] ERROR: No free NPC slots (MAX_NPCS reached).");
        return true;
    }

    /*Look for a random monster suitable for the plan*/
    int pool[1000];
    int psize = 0;
    for (int bi = 0; bi < bestiary_size; bi++) {
        if (target_floor >= bestiary_data[bi].floor_min &&
            target_floor <= bestiary_data[bi].floor_max) {
            if (psize < 1000) {
                pool[psize++] = bi;
            }
        }
    }

    if (psize == 0) {
        send_text_to_client(
            c->sock, "[DM] ERROR: No monsters in the bestiary are suitable for the %d floor.",
            target_floor);
        return true;
    }

    int boss_id = pool[rand() % psize];
    memset(b, 0, sizeof(NPC));
    /*active is set at the END of the setup, through npc_set_active: the
   * floor caches need the final fields.*/
    b->archetype = ARCH_BOSS;
    b->entity_id = game->next_id++;
    b->floor_id = target_floor;
    b->x = MAP_CENTER_X;
    b->y = MAP_CENTER_Y;
    b->spawn_x = MAP_CENTER_X;
    b->spawn_y = MAP_CENTER_Y;
    b->respawn_timer = 0;
    b->template = &bestiary_data[boss_id];
    b->hp = b->template->hp_avg * 5 + (target_floor * 50);
    b->max_hp = b->hp;
    snprintf(b->custom_name, sizeof(b->custom_name), "Boss %s", b->template->name);
    ai_init_npc(b, b->custom_name, b->floor_id);
    npc_set_active(game, b, true);

    send_text_to_client(c->sock, "[DM] Boss %s (Entity %d) successfully spawned on floor %d!",
                        b->custom_name, b->entity_id, target_floor);
    return true;
}

/*Everything a DM wants to know about one boss.*/
static void dm_boss_report(Client *c, const NPC *n, int ordinal) {
    const char *bname = (n->template) ? n->template->name : "Unknown Boss";
    send_text_to_client(c->sock, "[DM] ╔═══════════════════════ ═══════════════════════╗");
    send_text_to_client(c->sock, "[DM] BOSS #%d — %s (NPC id %d)", ordinal, bname, n->entity_id);
    send_text_to_client(c->sock, "[DM] ╚═══════════════════════ ═══════════════════════╝");
    send_text_to_client(c->sock, "[DM] Position: Floor %d X:%d Y:%d", n->floor_id, n->x, n->y);
    send_text_to_client(c->sock, "[DM] Spawn : X:%d Y:%d", n->spawn_x, n->spawn_y);
    /*Combat statistics*/
    send_text_to_client(c->sock, "[DM] HP : %d / %d", n->hp, n->max_hp);
    send_text_to_client(c->sock, "[DM] CA : %d", n->ac);
    send_text_to_client(c->sock, "[DM] Attack: +%d Damage: %dd%d", n->attack_bonus, n->damage_dice,
                        n->damage_sides);
    send_text_to_client(c->sock, "[DM] XP: %d Gold: %d", n->xp_reward, n->gold_drop);
    send_text_to_client(c->sock, "[DM] Moral: %d", n->morale);
    /*Data from template bestiary*/
    if (n->template) {
        const MonsterTemplate *t = n->template;
        send_text_to_client(c->sock, "[DM] View: %d Speed: %d", t->sight_range, t->speed);
        send_text_to_client(c->sock, "[DM] Floors : %d-%d", t->floor_min, t->floor_max);
        if (t->description && t->description[0] != '\0') {
            send_text_to_client(c->sock, "[DM] Lore : %.120s", t->description);
        }
    }
    /*Active spell slots*/
    bool has_slots = false;
    for (int sl = 1; sl <= MAX_SPELL_LEVEL; sl++) {
        if (n->spell_slots_max[sl] > 0) {
            has_slots = true;
            break;
        }
    }
    if (has_slots) {
        char slot_buf[256];
        slot_buf[0] = '\0';
        for (int sl = 1; sl <= MAX_SPELL_LEVEL; sl++) {
            if (n->spell_slots_max[sl] <= 0) {
                continue;
            }
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "L%d:%d/%d  ", sl, n->spell_slots[sl],
                     n->spell_slots_max[sl]);
            strncat(slot_buf, tmp, sizeof(slot_buf) - strlen(slot_buf) - 1);
        }
        send_text_to_client(c->sock, "[DM] Ki/Inc Slot: %s", slot_buf);
    }
    /*Loot ghost (items he carries)*/
    bool has_loot = false;
    for (int li = 0; li < 30; li++) {
        if (n->ghost_loot[li].template_idx >= 0 && n->ghost_loot[li].stack_count > 0) {
            has_loot = true;
            break;
        }
    }
    if (has_loot) {
        send_text_to_client(c->sock, "[DM] --- Inventory/Loot ---");
        for (int li = 0; li < 30; li++) {
            if (n->ghost_loot[li].template_idx < 0 || n->ghost_loot[li].stack_count <= 0) {
                continue;
            }
            char loot_buf[128];
            get_full_item_name(&n->ghost_loot[li], loot_buf, sizeof(loot_buf));
            send_text_to_client(c->sock, "[DM] [%d] %s", li, loot_buf);
        }
    } else {
        send_text_to_client(c->sock, "[DM] Inventory: empty (loot generated on death)");
    }
    /*Active effects*/
    if (n->effect_count > 0) {
        send_text_to_client(c->sock, "[DM] --- Active Effects (%d) ---", n->effect_count);
        for (int ei = 0; ei < n->effect_count; ei++) {
            const char *ename = n->effects[ei].name ? n->effects[ei].name : "?";
            send_text_to_client(c->sock, "[DM] [%d] %s val:%d duration:%d round", ei, ename,
                                n->effects[ei].value, n->effects[ei].duration_rounds);
        }
    }
    send_text_to_client(c->sock, "[DM] ──────────────────────── ────────────────────────");
}

static bool cmd_dm_boss(Client *c, const char *cmd, Game *game) {
    int target_floor = 0;
    if (!parse_int(cmd + 7, &target_floor, NULL)) {
        send_text_to_client(c->sock, "[DM] Use: dm_boss <floor> (ex: dm_boss 10)");
        send_text_to_client(c->sock, "[DM] Bosses appear every 10 floors: 10, 20, 30, ...");
        return true;
    }
    /*Bosses live on floors multiples of 10*/
    if (target_floor < 1 || target_floor >= MAX_FLOORS) {
        send_text_to_client(c->sock, "[DM] The dungeon has floors 1 to %d.", MAX_FLOORS - 1);
        return true;
    }
    if (target_floor <= 0 || target_floor % 10 != 0) {
        int nearest_below = (target_floor / 10) * 10;
        int nearest_above = nearest_below + 10;
        if (nearest_below <= 0) {
            send_text_to_client(c->sock,
                                "[DM] Floor %d is not a boss floor. The first boss floor is 10.",
                                target_floor);
        } else {
            send_text_to_client(
                c->sock, "[DM] Floor %d is not a boss floor (bosses appear every 10 floors).",
                target_floor);
            send_text_to_client(c->sock, "[DM] Adjacent boss floors: Floor %d or Floor %d",
                                nearest_below, nearest_above);
        }
        return true;
    }
    int boss_count = 0;
    FloorIter walk = floor_iter(game, target_floor);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype == ARCH_BOSS) {
            dm_boss_report(c, &game->npcs[i], ++boss_count);
        }
    }

    int dead_count = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (game->npcs[i].active) {
            continue;
        }
        if (game->npcs[i].floor_id != target_floor) {
            continue;
        }
        if (game->npcs[i].archetype != ARCH_BOSS) {
            continue;
        }
        if (game->npcs[i].respawn_timer <= 0) {
            continue;
        }
        dead_count++;
        const char *bname =
            (game->npcs[i].template) ? game->npcs[i].template->name : "Unknown Boss";
        send_text_to_client(c->sock,
                            "[DM] *** DEAD BOSS — %s (NPC id %d) — respawn in %d round ***", bname,
                            game->npcs[i].entity_id, game->npcs[i].respawn_timer);
        send_text_to_client(c->sock, "[DM] Spawn: X:%d Y:%d |  Max HP: %d", game->npcs[i].spawn_x,
                            game->npcs[i].spawn_y, game->npcs[i].max_hp);
    }

    if (boss_count == 0 && dead_count == 0) {
        send_text_to_client(c->sock,
                            "[DM] No bosses found on Floor %d (neither active nor respawning).",
                            target_floor);
    } else if (boss_count == 0) {
        send_text_to_client(c->sock, "[DM] Floor %d: Boss defeated, %d respawning.", target_floor,
                            dead_count);
    } else {
        send_text_to_client(c->sock, "[DM] Total active bosses on Floor %d: %d", target_floor,
                            boss_count);
    }
    return true;
}

static bool cmd_dm_merchant(Client *c, const char *cmd, Game *game) {
    int target_floor = 0;
    if (!parse_int(cmd + 12, &target_floor, NULL)) {
        send_text_to_client(c->sock, "[DM] Use: dm_merchant <floor> (ex: dm_merchant 5)");
        send_text_to_client(
            c->sock,
            "[DM] Wandering merchants appear every 5 floors (e.g. 5, 15, 25), excluding boss floors.");
        return true;
    }
    if (target_floor < 1 || target_floor >= MAX_FLOORS) {
        send_text_to_client(c->sock, "[DM] The dungeon has floors 1 to %d.", MAX_FLOORS - 1);
        return true;
    }
    /*Merchants appear every 5 floors, but not on multiples of 10 (boss floors)*/
    if (target_floor <= 0 || target_floor % 5 != 0 || target_floor % 10 == 0) {
        int nearest_below = target_floor - (target_floor % 5);
        if (nearest_below % 10 == 0) {
            nearest_below -= 5;
        }

        int nearest_above = target_floor + (5 - (target_floor % 5));
        if (nearest_above % 10 == 0) {
            nearest_above += 5;
        }

        if (nearest_below <= 0) {
            send_text_to_client(
                c->sock, "[DM] Floor %d does not have a merchant. The first merchant floor is 5.",
                target_floor);
        } else {
            send_text_to_client(
                c->sock,
                "[DM] Floor %d does not have a wandering merchant (they appear every 5 non-boss floors).",
                target_floor);
            send_text_to_client(c->sock, "[DM] Adjacent merchant floors: Floor %d or Floor %d",
                                nearest_below, nearest_above);
        }
        return true;
    }
    int merch_count = 0;
    FloorIter walk = floor_iter(game, target_floor);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (game->npcs[i].archetype != ARCH_MERCHANT) {
            continue;
        }
        merch_count++;
        send_text_to_client(c->sock, "[DM] ╔═══════════════════════ ═══════════════════════╗");
        send_text_to_client(c->sock, "[DM] WANDING MERCHANT #%d (NPC id %d)", merch_count,
                            game->npcs[i].entity_id);
        send_text_to_client(c->sock, "[DM] ╚═══════════════════════ ═══════════════════════╝");
        /* Position */
        send_text_to_client(c->sock, "[DM] Position: Floor %d X:%d Y:%d", game->npcs[i].floor_id,
                            game->npcs[i].x, game->npcs[i].y);
        /*Stats (a merchant usually doesn't fight, but we show it anyway)*/
        send_text_to_client(c->sock, "[DM] HP : %d / %d", game->npcs[i].hp, game->npcs[i].max_hp);
        send_text_to_client(c->sock, "[DM] Restock timer: %d",
                            game->npcs[i].merchant.restock_timer);

        /*Inventory for sale*/
        if (game->npcs[i].merchant.item_count > 0) {
            send_text_to_client(c->sock, "[DM] --- Goods for sale (%s) ---",
                                game->npcs[i].merchant.shop_name);
            for (int li = 0; li < game->npcs[i].merchant.item_count; li++) {
                int t_idx = game->npcs[i].merchant.item_templates[li];
                int stock = game->npcs[i].merchant.item_stock[li];
                int max_stock = game->npcs[i].merchant.item_stock_max[li];
                if (t_idx >= 0 && t_idx < item_database_size) {
                    send_text_to_client(c->sock, "[DM] [%d] %s (Quantity: %d/%d)", li,
                                        item_database[t_idx].name, stock, max_stock);
                }
            }
        } else {
            send_text_to_client(c->sock, "[DM] Goods for sale: empty");
        }
        send_text_to_client(c->sock, "[DM] ──────────────────────── ────────────────────────");
    }
    if (merch_count == 0) {
        send_text_to_client(c->sock, "[DM] No active wandering merchants found on Floor %d.",
                            target_floor);
    } else {
        send_text_to_client(c->sock, "[DM] Total merchants found on Floor %d: %d", target_floor,
                            merch_count);
    }
    return true;
}

/*A free tile of the DM's floor for dm_spawn / dm_place, or NULL after
 * telling the DM why not.*/
static Floor *dm_target_tile(Client *c, Game *game, int x, int y) {
    Floor *fl = &game->world->floors[c->floor_id];
    if (!tile_free(fl, x, y)) {
        send_text_to_client(c->sock, "[DM] %d,%d is not a free, walkable tile of this floor.", x,
                            y);
        return NULL;
    }
    return fl;
}

static bool cmd_dm_spawn(Client *c, const char *cmd, Game *game) {
    int v[3] = {0};
    if (scan_ints(cmd + 9, ' ', 3, v) != 3) {
        return false; /*usage*/
    }
    int tid = v[0], tx = v[1], ty = v[2];
    if (tid < 0 || tid >= bestiary_size) {
        send_text_to_client(c->sock, "[DM] Use: dm_spawn <bestiary_id 0-%d> <x> <y>",
                            bestiary_size - 1);
        return true;
    }
    Floor *fl = dm_target_tile(c, game, tx, ty);
    if (!fl) {
        return true;
    }
    NPC *n = npc_alloc(game);
    if (!n) {
        send_text_to_client(c->sock, "[DM] ERROR: No free NPC slots.");
        return true;
    }
    /*Nothing of the slot's previous tenant (loot, name, effects, timers).*/
    memset(n, 0, sizeof(*n));
    n->entity_id = game->next_id++;
    n->floor_id = c->floor_id;
    n->x = n->spawn_x = tx;
    n->y = n->spawn_y = ty;
    n->template_idx = tid;
    n->template = &bestiary_data[tid];
    n->hp = n->max_hp = n->template->hp_avg;
    ai_attach_behavior(n);
    fl->entity_grid[ty][tx] = n->entity_id;
    /*Activate only once the template is assigned: npc_set_active needs the
     * final fields.*/
    npc_set_active(game, n, true);
    send_text_to_client(c->sock, "[DM] Spawned %s at %d,%d", n->template->name, tx, ty);
    return true;
}

static bool cmd_dm_goto(Client *c, const char *cmd, Game *game) {
    (void)game;
    int v[3] = {0};
    if (scan_ints(cmd + 8, ' ', 3, v) == 3) {
        int f = v[0], tx = v[1], ty = v[2];
        /*Bounds: out-of-map coordinates would be used to index the map
     * and entity_grid on the very next tick (OOB read/write).*/
        if (f >= 0 && f < MAX_FLOORS && tx >= 0 && tx < MAP_WIDTH && ty >= 0 && ty < MAP_HEIGHT) {
            /*Keep entity_grid and the other players' views consistent with
       * the teleport.*/
            int old_floor = c->floor_id;
            Floor *ofl = &game->world->floors[old_floor];
            if (ofl->entity_grid[c->y][c->x] == c->entity_id) {
                ofl->entity_grid[c->y][c->x] = 0;
            }
            c->floor_id = f;
            client_track_explored_floor(c);
            c->x = tx;
            c->y = ty;
            game->world->floors[f].entity_grid[ty][tx] = c->entity_id;
            if (old_floor != f) {
                notify_player_left_floor(game, c, old_floor);
            }
            send_text_to_client(c->sock, "[DM] Teleported to Floor %d (%d,%d)", f, tx, ty);
            // Force map refresh
            send_map_chunk(c->sock, &game->world->floors[f].map, tx, ty, 15);
            return true;
        }
        send_text_to_client(c->sock,
                            "[DM] Out of bounds. Use: dm_goto <floor 0-%d> <x 0-%d> <y 0-%d>",
                            MAX_FLOORS - 1, MAP_WIDTH - 1, MAP_HEIGHT - 1);
        return true;
    }
    return false; /*usage*/
}

static bool cmd_dm_pdf(Client *c, const char *cmd, Game *game) {
    (void)game;
    int f = 0;
    if (parse_int(cmd + 7, &f, NULL) && f >= 0 && f < MAX_FLOORS) {
        dm_pdf_map(game, c, f);
    } else {
        send_text_to_client(c->sock, "[DM] Use: dm_pdf <floor 0-%d>", MAX_FLOORS - 1);
    }
    return true;
}

static bool cmd_dm_mapfloor(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    dm_pdf_map(game, c, c->floor_id);
    return true;
}

static bool cmd_dm_shop(Client *c, const char *cmd, Game *game) {
    int v[2] = {0, 1};
    if (scan_ints(cmd + 8, ' ', 2, v) < 1) {
        return false; /*usage*/
    }
    int iid = v[0], qty = v[1];
    if (iid < 0 || iid >= item_database_size || qty < 1) {
        send_text_to_client(c->sock, "[DM] Use: dm_shop <item_id 0-%d> [quantity >= 1]",
                            item_database_size - 1);
        return true;
    }
    {
        FloorIter walk = floor_iter(game, c->floor_id);
        for (int i; (i = floor_iter_next(&walk)) >= 0;) {
            if (game->npcs[i].archetype == ARCH_MERCHANT) {
                if (abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y) <= 5) {
                    add_item_to_shop(&game->npcs[i], iid, qty);
                    send_text_to_client(c->sock, "[DM] Added item %d (qty %d) to %s", iid, qty,
                                        game->npcs[i].merchant.shop_name);
                    return true;
                }
            }
        }
    }
    send_text_to_client(c->sock, "[DM] No merchant within 5 steps.");
    return true;
}

static bool cmd_dm_place(Client *c, const char *cmd, Game *game) {
    int v[3] = {0};
    if (scan_ints(cmd + 9, ' ', 3, v) != 3) {
        return false; /*usage*/
    }
    int iid = v[0], tx = v[1], ty = v[2];
    if (iid < 0 || iid >= item_database_size) {
        send_text_to_client(c->sock, "[DM] Use: dm_place <item_id 0-%d> <x> <y>",
                            item_database_size - 1);
        return true;
    }
    if (!dm_target_tile(c, game, tx, ty)) {
        return true;
    }
    const ItemTemplate *it = &item_database[iid];
    ItemInstance item;
    memset(&item, 0, sizeof(item));
    item.template_idx = iid;
    item.durability = it->max_durability > 0 ? it->max_durability : 100;
    item.stack_count = 1;
    item.is_identified = true;
    if (!npc_spawn_item(game, c->floor_id, tx, ty, &item)) {
        send_text_to_client(c->sock, "[DM] ERROR: No free NPC slots.");
        return true;
    }
    send_text_to_client(c->sock, "[DM] Placed %s at %d,%d", it->name, tx, ty);
    return true;
}

static const CmdEntry DM_COMMANDS[] = {
    {NULL, "dm_gold ", cmd_dm_gold, "dm_gold <amount>"},
    {NULL, "dm_level ", cmd_dm_level, "dm_level <level>"},
    {NULL, "dm_find_monster ", cmd_dm_find_monster, "dm_find_monster <name>"},
    {NULL, "dm_find_item ", cmd_dm_find_item, "dm_find_item <name>"},
    {"dm_telemetry", NULL, cmd_dm_telemetry, NULL},
    {"dm_spawn_boss", "dm_spawn_boss ", cmd_dm_spawn_boss, NULL},
    {"dm_boss", "dm_boss ", cmd_dm_boss, NULL},
    {NULL, "dm_merchant ", cmd_dm_merchant, "dm_merchant <floor>"},
    {NULL, "dm_spawn ", cmd_dm_spawn, "dm_spawn <bestiary_id> <x> <y>"},
    {NULL, "dm_goto ", cmd_dm_goto, "dm_goto <floor> <x> <y>"},
    {NULL, "dm_pdf ", cmd_dm_pdf, "dm_pdf <floor>"},
    {"dm_mapfloor", NULL, cmd_dm_mapfloor, NULL},
    {NULL, "dm_shop ", cmd_dm_shop, "dm_shop <item_id> [quantity]"},
    {NULL, "dm_place ", cmd_dm_place, "dm_place <item_id> <x> <y>"},
};

/*Runs a dm_* command; false (not handled) for any other account or
 * command, so the player commands are tried next.*/
const char *cmd_dm_usage(const char *cmd) {
    const CmdEntry *e =
        cmd_table_find_word(DM_COMMANDS, sizeof(DM_COMMANDS) / sizeof(DM_COMMANDS[0]), cmd);
    return e ? e->usage : NULL;
}

bool cmd_dm(Client *c, const char *cmd, Game *game) {
    if (!c->is_dm) {
        return false;
    }
    return cmd_table_run(DM_COMMANDS, sizeof(DM_COMMANDS) / sizeof(DM_COMMANDS[0]), c, cmd, game);
}
