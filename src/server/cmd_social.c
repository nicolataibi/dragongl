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
 * cmd_social.c - Social commands: leaderboard, event, party, player-to-player trade,
 * private and local chat, block list.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "persist.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>

typedef struct {
    char username[32];
    int level;
    int xp;
    uint32_t bosses_defeated;
} LeaderboardEntry;

static int compare_leaderboard(const void *a, const void *b) {
    const LeaderboardEntry *ea = (const LeaderboardEntry *)a;
    const LeaderboardEntry *eb = (const LeaderboardEntry *)b;
    //Primary Sorting: Number of bosses defeated
    int bosses_a = __builtin_popcount(ea->bosses_defeated);
    int bosses_b = __builtin_popcount(eb->bosses_defeated);
    if (bosses_b != bosses_a) return bosses_b - bosses_a;
    //Secondary sorting: level
    if (eb->level != ea->level) return eb->level - ea->level;
    //Tertiary sorting: XP
    return eb->xp - ea->xp;
}

/*--- Leaderboard ('top') ----------------------------------------------
 * Reading and checking every save file on each 'top' made a plain player
 * command cost one disk read per character ever created, inside a game
 * tick. The saved characters are scanned at most once per
 * LEADERBOARD_RESCAN_S; the characters in play are taken live from
 * memory, so they are always current.*/
#define LEADERBOARD_MAX 1000
#define LEADERBOARD_RESCAN_S 60

static LeaderboardEntry g_board[LEADERBOARD_MAX];
static int g_board_count;
static time_t g_board_scanned; /*0 = never*/
static bool g_board_ok;

static void leaderboard_rescan(void) {
  g_board_count = 0;
  g_board_ok = false;
  char saves_dir[STATE_DIR_MAX + 16];
  DIR *d = state_path(saves_dir, sizeof(saves_dir), "saves") ? opendir(saves_dir) : NULL;
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && g_board_count < LEADERBOARD_MAX) {
    size_t len = strlen(de->d_name);
    if (len <= 5 || strcmp(de->d_name + len - 5, ".save") != 0 || len - 5 >= 32)
      continue;
    char username[32];
    memcpy(username, de->d_name, len - 5);
    username[len - 5] = '\0';
    if (strcmp(username, "dm") == 0)
      continue;
    char filepath[STATE_DIR_MAX + 300];
    if (snprintf(filepath, sizeof(filepath), "%s/%s", saves_dir, de->d_name) >=
        (int)sizeof(filepath))
      continue;
    /*Only readable saves are listed: a corrupt one (which login would
     * refuse anyway) would show garbage.*/
    SaveData sd;
    if (persist_read_player(filepath, &sd, NULL, 0) != BINFILE_OK)
      continue;
    LeaderboardEntry *e = &g_board[g_board_count++];
    copy_str(e->username, username, sizeof(e->username));
    e->level = sd.level;
    e->xp = sd.xp;
    e->bosses_defeated = sd.bosses_defeated;
    auth_wipe(&sd, sizeof(sd));
  }
  closedir(d);
  g_board_ok = true;
}

static void show_leaderboard(Client *c) {
  time_t now = time(NULL);
  if (g_board_scanned == 0 || now - g_board_scanned >= LEADERBOARD_RESCAN_S || now < g_board_scanned) {
    leaderboard_rescan();
    g_board_scanned = now;
  }
  if (!g_board_ok) {
    send_text_to_client(c->sock, "[ERROR] Unable to read save files.");
    return;
  }
  static LeaderboardEntry entries[LEADERBOARD_MAX + MAX_CLIENTS];
  int n = g_board_count;
  memcpy(entries, g_board, sizeof(LeaderboardEntry) * (size_t)n);
  /*Characters in play: their live values replace the (older) saved ones.*/
  for (int i = 0; i < MAX_CLIENTS; i++) {
    const Client *p = &g_clients[i];
    if (!p->active || !p->authenticated || p->is_dm)
      continue;
    LeaderboardEntry *e = NULL;
    for (int k = 0; k < n; k++)
      if (strcmp(entries[k].username, p->username) == 0) {
        e = &entries[k];
        break;
      }
    if (!e)
      e = &entries[n++];
    copy_str(e->username, p->username, sizeof(e->username));
    e->level = p->level;
    e->xp = p->xp;
    e->bosses_defeated = p->bosses_defeated;
  }
  qsort(entries, (size_t)n, sizeof(LeaderboardEntry), compare_leaderboard);
  send_text_to_client(c->sock, "=================== LEADERBOARD ===================");
  for (int i = 0; i < n && i < 10; i++) {
    int bosses = __builtin_popcount(entries[i].bosses_defeated);
    send_text_to_client(c->sock, "%d. %s (Level %d) - %d XP - %d Bosses Defeated", i + 1,
                        entries[i].username, entries[i].level, entries[i].xp, bosses);
  }
  send_text_to_client(c->sock, "===================================================");
}

/* ====================================================================
 * SISTEMA MESSAGGISTICA (msg/tell/whisper, say, who, block/unblock)
 *
* Private messages and local chat go through the MSG_TEXT channel
* existing: the client does not require any changes. The list of
* block is persistent (SaveData) and prevents both messages
* both private and the blocked player's local chat.
 * ==================================================================== */

#define MSG_LOCAL_RADIUS 12     /*Radius (Chebyshev distance) for 'say'*/
#define MSG_MIN_INTERVAL_MS 500 /*Minimum interval between two msg/say (anti-spam)*/

/*Find a player online by username (case-insensitive).
*Returns NULL if absent or unauthenticated.*/
static Client *find_online_player(const char *name) {
  if (!name || name[0] == '\0')
    return NULL;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated &&
        strcasecmp(g_clients[i].username, name) == 0) {
      return &g_clients[i];
    }
  }
  return NULL;
}

/*True if 'listener' has 'speaker' in its block list.
*DMs cannot be blocked.*/
static bool player_is_blocked(const Client *listener, const char *speaker) {
  if (listener->is_dm)
    return false;
  for (int i = 0; i < listener->blocked_count; i++) {
    if (strcasecmp(listener->blocked_players[i], speaker) == 0)
      return true;
  }
  return false;
}

/*Anti-spam: true if the client can send another message now.*/
static bool msg_rate_ok(Client *c) {
  long long now = get_time_ms();
  if (now - c->last_msg_ms < MSG_MIN_INTERVAL_MS)
    return false;
  c->last_msg_ms = now;
  return true;
}

/*Removes leading and trailing spaces in-place. Returns the beginning of the string.*/
static char *trim_spaces(char *s) {
  while (*s == ' ')
    s++;
  char *end = s + strlen(s);
  while (end > s && end[-1] == ' ')
    *--end = '\0';
  return s;
}

/*--- Player-to-player trade -----------------------------------------
 * 'offer' records the backpack index AND a byte copy of the item; the
 * exchange runs only when both players have accepted, and trade_check()
 * re-validates everything at that moment. Any change to an offered item
 * in between (sold, dropped, used, shifted by another removal) cancels
 * the trade instead of moving a different item or a stale copy.*/
static void trade_reset(Client *c) {
  c->trading_with[0] = '\0';
  c->trade_offer_item_idx = -1;
  c->trade_offer_gold = 0;
  c->trade_accepted = false;
}

static bool trade_offer_intact(const Client *c) {
  int idx = c->trade_offer_item_idx;
  if (idx == -1)
    return true;
  return idx >= 0 && idx < c->backpack_count &&
         memcmp(&c->backpack[idx], &c->trade_offer_item, sizeof(ItemInstance)) == 0;
}

/*NULL if the trade can run, otherwise the reason it cannot.*/
static const char *trade_check(const Client *c, const Client *t) {
  if (!trade_offer_intact(c) || !trade_offer_intact(t))
    return "an offered item has changed or is gone";
  if (c->gold < c->trade_offer_gold || t->gold < t->trade_offer_gold)
    return "insufficient gold";
  if (c->floor_id != t->floor_id || abs(c->x - t->x) > 2 || abs(c->y - t->y) > 2)
    return "you are too far apart";
  int c_gives = (c->trade_offer_item_idx != -1), t_gives = (t->trade_offer_item_idx != -1);
  if (c->backpack_count - c_gives + t_gives > MAX_BACKPACK ||
      t->backpack_count - t_gives + c_gives > MAX_BACKPACK)
    return "a backpack is full";
  return NULL;
}

static void backpack_remove_at(Client *c, int idx) {
  for (int k = idx; k < c->backpack_count - 1; k++)
    c->backpack[k] = c->backpack[k + 1];
  c->backpack_count--;
  memset(&c->backpack[c->backpack_count], 0, sizeof(ItemInstance));
  c->backpack[c->backpack_count].template_idx = -1;
}

/*Precondition: trade_check(c, t) == NULL.*/
static void trade_execute(Client *c, Client *t) {
  c->gold = c->gold - c->trade_offer_gold + t->trade_offer_gold;
  t->gold = t->gold - t->trade_offer_gold + c->trade_offer_gold;
  bool c_gives = (c->trade_offer_item_idx != -1), t_gives = (t->trade_offer_item_idx != -1);
  ItemInstance from_c = c->trade_offer_item, from_t = t->trade_offer_item;
  if (c_gives)
    backpack_remove_at(c, c->trade_offer_item_idx);
  if (t_gives)
    backpack_remove_at(t, t->trade_offer_item_idx);
  if (c_gives)
    t->backpack[t->backpack_count++] = from_c;
  if (t_gives)
    c->backpack[c->backpack_count++] = from_t;
  save_player_data(c);
  save_player_data(t);
}

//top, leaderboard — Shows player rankings
bool cmd_top(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    show_leaderboard(c);
    return true;
}

//--- GLOBAL EVENT --- displays the current event
bool cmd_event(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (active_event_type == 0) {
        send_text_to_client(c->sock, "[EVENT] No global events active at the moment.");
    } else {
        int mins_left = (event_time_left * 200) / 60000;
        int secs_left = ((event_time_left * 200) % 60000) / 1000;
        send_text_to_client(c->sock, "[GLOBAL EVENT] Invasion of Plane %d!", event_floor_id);
        send_text_to_client(c->sock, "  Goal: kill %d Skeletons — Progress: %d/%d", event_goal, event_progress, event_goal);
        send_text_to_client(c->sock, "  Time left: %d min %d sec", mins_left, secs_left);
    }
    return true;
}

//--- PARTY SYSTEM ---
bool cmd_invite(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
    char target_name[32];
    strncpy(target_name, cmd + 7, 31); target_name[31] = '\0';
    bool found = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && strcmp(g_clients[i].username, target_name) == 0) {
            copy_str(g_clients[i].pending_invite, c->username, sizeof(g_clients[i].pending_invite));
            send_text_to_client(g_clients[i].sock, "[PARTY] %s has invited you to his party! Type 'join' to accept.", c->username);
            send_text_to_client(c->sock, "[PARTY] Invitation sent to %s.", target_name);
            found = true;
            break;
        }
    }
    if (!found) send_text_to_client(c->sock, "[PARTY] Player %s not found online.", target_name);
    return true;
}

bool cmd_join(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (strlen(c->pending_invite) == 0) {
        send_text_to_client(c->sock, "[PARTY] No pending invitations.");
        return true;
    }
    //If not already a leader of another party (if so, cannot join unless they leave first)
    if (strlen(c->party_leader) > 0 && strcmp(c->party_leader, c->username) == 0) {
        send_text_to_client(c->sock, "[PARTY] You are the leader of your party. Leave it with 'leave' before joining another.");
        return true;
    }
    bool leader_online = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && strcmp(g_clients[i].username, c->pending_invite) == 0) {
            leader_online = true;
            // Set the leader of the person who invited us as our leader
            if (strlen(g_clients[i].party_leader) > 0) {
                copy_str(c->party_leader, g_clients[i].party_leader, sizeof(c->party_leader));
            } else {
                //They didn't have a party, so they become the leader
                strncpy(g_clients[i].party_leader, g_clients[i].username, 31);
                strncpy(c->party_leader, g_clients[i].username, 31);
            }
            send_text_to_client(c->sock, "[PARTY] You have joined %s' party!", c->party_leader);
            send_text_to_client(g_clients[i].sock, "[PARTY] %s has joined the party!", c->username);
            c->pending_invite[0] = '\0';
            break;
        }
    }
    if (!leader_online) {
        send_text_to_client(c->sock, "[PARTY] The player who invited you is no longer online.");
        c->pending_invite[0] = '\0';
    }
    return true;
}

bool cmd_leave(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (strlen(c->party_leader) == 0) {
        send_text_to_client(c->sock, "[PARTY] You are not in any party.");
        return true;
    }
    send_text_to_client(c->sock, "[PARTY] You left the party.");
    // If the leader leaves, the party is dissolved
    if (strcmp(c->party_leader, c->username) == 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->username) == 0 && strcmp(g_clients[i].username, c->username) != 0) {
                send_text_to_client(g_clients[i].sock, "[PARTY] The leader has disbanded the party.");
                g_clients[i].party_leader[0] = '\0';
            }
        }
    } else {
        // Notify others that they left
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0 && strcmp(g_clients[i].username, c->username) != 0) {
                send_text_to_client(g_clients[i].sock, "[PARTY] %s has left the party.", c->username);
            }
        }
    }
    c->party_leader[0] = '\0';
    return true;
}

bool cmd_party(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (strlen(c->party_leader) == 0) {
        send_text_to_client(c->sock, "[PARTY] You are not in any party. Create one by inviting: invite <name>");
        return true;
    }
    send_text_to_client(c->sock, "=== PARTY MEMBERS (Leaders: %s) ===", c->party_leader);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0) {
            send_text_to_client(c->sock, "- %s (Lvl %d, %d/%d HP)", g_clients[i].username, g_clients[i].level, g_clients[i].hp, g_clients[i].max_hp);
        }
    }
    return true;
}

// --- TRADE SYSTEM P2P ---
bool cmd_trade(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
    char target_name[32];
    strncpy(target_name, cmd + 6, 31); target_name[31] = '\0';

    if (strcmp(c->username, target_name) == 0) {
        send_text_to_client(c->sock, "[TRADE] You cannot trade with yourself.");
        return true;
    }

    bool found = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *t = &g_clients[i];
        if (t->active && t->authenticated && strcmp(t->username, target_name) == 0) {
            if (abs(t->x - c->x) > 2 || abs(t->y - c->y) > 2 || t->floor_id != c->floor_id) {
                send_text_to_client(c->sock, "[TRADE] %s is too far away.", target_name);
                return true;
            }

            copy_str(c->trading_with, target_name, sizeof(c->trading_with));
            c->trade_offer_item_idx = -1;
            c->trade_offer_gold = 0;
            c->trade_accepted = false;

            if (strcmp(t->trading_with, c->username) == 0) {
                send_text_to_client(c->sock, "[TRADE] Trade started with %s. Use 'offer <gold> [object_id]' and 'accept'.", target_name);
                send_text_to_client(t->sock, "[TRADE] Trade started with %s. Use 'offer <gold> [object_id]' and 'accept'.", c->username);
            } else {
                send_text_to_client(t->sock, "[TRADE] %s wants to trade with you. Type 'trade %s' to get started.", c->username, c->username);
                send_text_to_client(c->sock, "[TRADE] Trade request sent to %s.", target_name);
            }
            found = true;
            break;
        }
    }
    if (!found) send_text_to_client(c->sock, "[TRADE] Player %s not found.", target_name);
    return true;
}

bool cmd_offer(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
    if (strlen(c->trading_with) == 0) {
        send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
        return true;
    }
    Client *t = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && strcmp(g_clients[i].username, c->trading_with) == 0) {
            t = &g_clients[i];
            break;
        }
    }
    if (!t || strcmp(t->trading_with, c->username) != 0) {
        send_text_to_client(c->sock, "[TRADE] The other player canceled or disconnected.");
        c->trading_with[0] = '\0';
        return true;
    }

    long long gold = 0;
    int item = 0;
    int parsed = sscanf(cmd + 6, "%lld %d", &gold, &item);
    if (parsed >= 1) {
        if (gold < 0 || c->gold < (uint64_t)gold) {
            send_text_to_client(c->sock, "[TRADE] You don't have enough gold.");
            return true;
        }
        c->trade_offer_gold = gold;
        c->trade_offer_item_idx = -1;
        if (parsed == 2) {
            int idx = item - 1;
            if (idx >= 0 && idx < c->backpack_count && c->backpack[idx].template_idx >= 0) {
                c->trade_offer_item_idx = idx;
                memcpy(&c->trade_offer_item, &c->backpack[idx], sizeof(ItemInstance));
            } else {
                send_text_to_client(c->sock, "[TRADE] Invalid item in backpack.");
                return true;
            }
        }
        c->trade_accepted = false;
        t->trade_accepted = false;

        char item_name[64] = "None";
        if (c->trade_offer_item_idx != -1) {
            strncpy(item_name, item_database[c->backpack[c->trade_offer_item_idx].template_idx].name, 63);
        }
        send_text_to_client(c->sock, "[TRADE] You bid: %lld gold, %s. Pending...", gold, item_name);
        send_text_to_client(t->sock, "[TRADE] %s offers: %lld gold, %s.", c->username, gold, item_name);
    }
    return true;
}

bool cmd_accept(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (strlen(c->trading_with) == 0) {
        send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
        return true;
    }
    Client *t = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated &&
            strcmp(g_clients[i].username, c->trading_with) == 0) {
            t = &g_clients[i];
            break;
        }
    }
    if (!t || strcmp(t->trading_with, c->username) != 0) {
        send_text_to_client(c->sock, "[TRADE] The other player canceled or disconnected.");
        c->trading_with[0] = '\0';
        return true;
    }

    c->trade_accepted = true;
    send_text_to_client(c->sock, "[TRADE] You accepted the offer.");
    send_text_to_client(t->sock, "[TRADE] %s accepted the offer.", c->username);

    if (c->trade_accepted && t->trade_accepted) {
        /*Everything is re-validated now: an offer is only a promise made
         * earlier, and both inventories may have changed since.*/
        const char *why = trade_check(c, t);
        if (why) {
            send_text_to_client(c->sock, "[TRADE] Trade cancelled: %s.", why);
            send_text_to_client(t->sock, "[TRADE] Trade cancelled: %s.", why);
        } else {
            trade_execute(c, t);
            send_text_to_client(c->sock, "[TRADE] Trade complete!");
            send_text_to_client(t->sock, "[TRADE] Trade complete!");
        }
        trade_reset(c);
        trade_reset(t);
    }
    return true;
}

bool cmd_cancel(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
    if (strlen(c->trading_with) == 0) {
        send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
        return true;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && strcmp(g_clients[i].username, c->trading_with) == 0) {
            send_text_to_client(g_clients[i].sock, "[TRADE] %s canceled the trade.", c->username);
            g_clients[i].trading_with[0] = '\0';
            break;
        }
    }
    c->trading_with[0] = '\0';
    send_text_to_client(c->sock, "[TRADE] Trade cancelled.");
    return true;
}

//--- MESSAGING SYSTEM BETWEEN PLAYERS ---

/*Messaggio privato: msg/tell/whisper <name> <text>*/
bool cmd_msg(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  int pfx = 0;
  if (strncmp(cmd, "msg ", 4) == 0)
    pfx = 3;
  else if (strncmp(cmd, "tell ", 5) == 0)
    pfx = 4;
  else if (strncmp(cmd, "whisper ", 8) == 0)
    pfx = 7;

  if (pfx > 0) {
    const char *rest = cmd + pfx;
    while (*rest == ' ')
      rest++;
    char target_name[32];
    int nlen = 0;
    while (nlen < 31 && rest[nlen] != '\0' && rest[nlen] != ' ') {
      target_name[nlen] = rest[nlen];
      nlen++;
    }
    target_name[nlen] = '\0';
    const char *text = rest + nlen;
    while (*text == ' ')
      text++;

    if (nlen == 0 || text[0] == '\0') {
      send_text_to_client(c->sock,
                          "[MSG] Usage: msg <name> <text> — e.g. 'msg Bob hello'");
      return true;
    }
    if (strcasecmp(target_name, c->username) == 0) {
      send_text_to_client(c->sock, "[MSG] You cannot message yourself.");
      return true;
    }
    Client *t = find_online_player(target_name);
    if (!t) {
      send_text_to_client(c->sock, "[MSG] Player %s is not online.",
                          target_name);
      return true;
    }
    if (!msg_rate_ok(c)) {
      send_text_to_client(c->sock,
                          "[MSG] You are typing too fast. Wait a moment.");
      return true;
    }
    /*If the recipient has blocked us (and we are not DM),
*the message is not delivered.*/
    if (!c->is_dm && player_is_blocked(t, c->username)) {
      send_text_to_client(c->sock,
                          "[MSG] Your message was not delivered.");
      return true;
    }
    send_text_to_client(t->sock, "[MSG] %s: %s", c->username, text);
    send_text_to_client(c->sock, "[MSG] -> %s: %s", t->username, text);
    return true;
  }
  return false;
}

/*Chat locale: say/shout <text> — sentita dai giocatori dello stesso
*floor within MSG_LOCAL_RADIUS, barring blocks.*/
bool cmd_say(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  int pfx = 0;
  if (strncmp(cmd, "say ", 4) == 0)
    pfx = 3;
  else if (strncmp(cmd, "shout ", 6) == 0)
    pfx = 5;

  if (pfx > 0) {
    char text[256];
    strncpy(text, cmd + pfx, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    char *ttext = trim_spaces(text);
    if (ttext[0] == '\0') {
      send_text_to_client(c->sock,
                          "[CHAT] Usage: say <text> — e.g. 'say anyone here?'");
      return true;
    }
    if (!msg_rate_ok(c)) {
      send_text_to_client(c->sock,
                          "[CHAT] You are typing too fast. Wait a moment.");
      return true;
    }
    int heard = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      Client *t = &g_clients[i];
      if (!t->active || !t->authenticated || t == c)
        continue;
      if (t->floor_id != c->floor_id)
        continue;
      int dx = abs(t->x - c->x), dy = abs(t->y - c->y);
      if (dx > MSG_LOCAL_RADIUS || dy > MSG_LOCAL_RADIUS)
        continue;
      if (player_is_blocked(t, c->username))
        continue; /*Blocked: Can't hear local chat*/
      send_text_to_client(t->sock, "[CHAT] %s: %s", c->username, ttext);
      heard++;
    }
    send_text_to_client(c->sock, "[CHAT] %s (you): %s  [%d nearby]",
                        c->username, ttext, heard);
    return true;
  }
  return false;
}

/*who — "list of players online" */
bool cmd_who(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  int count = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (g_clients[i].active && g_clients[i].authenticated)
      count++;
  send_text_to_client(c->sock, "[WHO] %d player(s) online:", count);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated) {
      send_text_to_client(
          c->sock, "  - %s (floor %d)%s", g_clients[i].username,
          g_clients[i].floor_id,
          strcmp(g_clients[i].username, c->username) == 0 ? " (you)" : "");
    }
  }
  return true;
}

/*block <name> — add a player to the block list.
*Works even if the player is offline: the name comes
*stored like this the block is valid when you return to play.*/
bool cmd_block(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  char name[32];
  strncpy(name, cmd + 6, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  char *n = trim_spaces(name);
  if (n[0] == '\0') {
    send_text_to_client(c->sock, "[BLOCK] Usage: block <name>");
    return true;
  }
  if (strcasecmp(n, c->username) == 0) {
    send_text_to_client(c->sock, "[BLOCK] You cannot block yourself.");
    return true;
  }
  if (player_is_blocked(c, n)) {
    send_text_to_client(c->sock, "[BLOCK] %s is already blocked.", n);
    return true;
  }
  if (c->blocked_count >= MAX_BLOCKED_PLAYERS) {
    send_text_to_client(
        c->sock,
        "[BLOCK] Block list is full (%d). Use 'unblock <name>' first.",
        MAX_BLOCKED_PLAYERS);
    return true;
  }
  char *slot = c->blocked_players[c->blocked_count];
  memset(slot, 0, sizeof(c->blocked_players[0]));
  copy_str(slot, n, sizeof(c->blocked_players[0]));
  c->blocked_count++;
  save_player_data(c);
  send_text_to_client(
      c->sock,
      "[BLOCK] %s has been blocked. Their messages will no longer reach you.",
      n);
  return true;
}

/*unblock <name> — remove a player from the block list*/
bool cmd_unblock(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  char name[32];
  strncpy(name, cmd + 8, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  char *n = trim_spaces(name);
  if (n[0] == '\0') {
    send_text_to_client(c->sock, "[BLOCK] Usage: unblock <name>");
    return true;
  }
  for (int i = 0; i < c->blocked_count; i++) {
    if (strcasecmp(c->blocked_players[i], n) == 0) {
      /*Move subsequent items to compact the list*/
      for (int k = i; k < c->blocked_count - 1; k++)
        memcpy(c->blocked_players[k], c->blocked_players[k + 1], 32);
      c->blocked_count--;
      save_player_data(c);
      send_text_to_client(c->sock, "[BLOCK] %s has been unblocked.", n);
      return true;
    }
  }
  send_text_to_client(c->sock, "[BLOCK] %s is not in your block list.", n);
  return true;
}

/*blocks — shows the block list*/
bool cmd_blocks(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->blocked_count == 0) {
    send_text_to_client(c->sock,
                        "[BLOCK] Your block list is empty. Use 'block <name>'.");
    return true;
  }
  send_text_to_client(c->sock, "[BLOCK] Blocked players (%d):",
                      c->blocked_count);
  for (int i = 0; i < c->blocked_count; i++)
    send_text_to_client(c->sock, "  - %s", c->blocked_players[i]);
  return true;
}
