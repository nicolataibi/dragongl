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
 * server_commands.c - Text commands of players and of the Dungeon Master.
 *
 * The commands are a table: the handlers live in one file per group
 * (cmd_character.c, cmd_social.c, cmd_shop.c, cmd_items.c, cmd_magic.c,
 * cmd_world.c, cmd_dm.c). The entries are tried in order and the order
 * matters: a prefix entry wins over every later entry it covers.
 */

#include "server_commands.h"
#include "server_cmd.h"
#include "server_internal.h"
#include <string.h>

static bool list_match(const char *list, const char *cmd, bool prefix) {
  if (!list)
    return false;
  for (const char *p = list;;) {
    const char *bar = strchr(p, '|');
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    if (strncmp(cmd, p, n) == 0 && (prefix || cmd[n] == '\0'))
      return true;
    if (!bar)
      return false;
    p = bar + 1;
  }
}

bool cmd_table_run(const CmdEntry *table, size_t n, Client *c, const char *cmd, NPC *npcs) {
  for (size_t i = 0; i < n; i++) {
    const CmdEntry *e = &table[i];
    bool any = !e->names && !e->prefixes;
    if ((any || list_match(e->names, cmd, false) || list_match(e->prefixes, cmd, true)) &&
        e->run(c, cmd, npcs))
      return true;
  }
  return false;
}

/*True if the first word of cmd (up to a space or its end) equals word[0..len).*/
static bool first_word_is(const char *cmd, const char *word, size_t len) {
  return len > 0 && strncmp(cmd, word, len) == 0 && (cmd[len] == '\0' || cmd[len] == ' ');
}

static bool list_has_word(const char *list, const char *cmd) {
  if (!list)
    return false;
  for (const char *p = list;;) {
    const char *bar = strchr(p, '|');
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    size_t word = (n > 0 && p[n - 1] == ' ') ? n - 1 : n;
    if (first_word_is(cmd, p, word))
      return true;
    if (!bar)
      return false;
    p = bar + 1;
  }
}

const CmdEntry *cmd_table_find_word(const CmdEntry *table, size_t n, const char *cmd) {
  for (size_t i = 0; i < n; i++)
    if (list_has_word(table[i].names, cmd) || list_has_word(table[i].prefixes, cmd))
      return &table[i];
  return NULL;
}

static const CmdEntry COMMANDS[] = {
    {"help|?", NULL, cmd_help, NULL},
    {"train", "train ", cmd_train, "train <str|dex|con|int|wis|cha|random>"},
    {"top|leaderboard", NULL, cmd_top, NULL},
    {"event", NULL, cmd_event, NULL},
    {NULL, "invite ", cmd_invite, "invite <player>"},
    {"join", NULL, cmd_join, NULL},
    {"leave", NULL, cmd_leave, NULL},
    {"party", NULL, cmd_party, NULL},
    {NULL, "trade ", cmd_trade, "trade <player>"},
    {NULL, "offer ", cmd_offer, "offer <gold> [backpack item n]"},
    {"accept", NULL, cmd_accept, NULL},
    {"cancel", NULL, cmd_cancel, NULL},
    {NULL, "msg |tell |whisper ", cmd_msg, "msg <player> <text>"},
    {NULL, "say |shout ", cmd_say, "say <text>"},
    {"who", NULL, cmd_who, NULL},
    {NULL, "block ", cmd_block, "block <player>"},
    {NULL, "unblock ", cmd_unblock, "unblock <player>"},
    {"blocks|blocklist", NULL, cmd_blocks, NULL},
    {NULL, "browse ", cmd_browse, "browse <n>"},
    {NULL, NULL, cmd_dm, NULL},
    {"retrieve", NULL, cmd_retrieve, NULL},
    {"list", NULL, cmd_list, NULL},
    {"look|l|lock", "look |lock ", cmd_look, NULL},
    {"fountain", NULL, cmd_fountain, NULL},
    {"statue|crystal", NULL, cmd_statue, NULL},
    {"status|stats", NULL, cmd_stats, NULL},
    {"inventory|i", NULL, cmd_inventory, NULL},
    {NULL, "wear |equip |wield |w ", cmd_wear, "wear <n|item>"},
    {NULL, "remove |unequip |takeoff ", cmd_remove, "remove <slot|item>"},
    {NULL, "buy ", cmd_buy, "buy <n|item>"},
    {NULL, "sell ", cmd_sell, "sell <n|item>"},
    {NULL, "haggle ", cmd_haggle, "haggle buy|sell <n|item>"},
    {"identify", "identify ", cmd_identify, NULL},
    {"cure", NULL, cmd_cure, NULL},
    {NULL, "belt ", cmd_belt, "belt <n>"},
    {"unbelt", "unbelt ", cmd_unbelt, NULL},
    {"repair", "repair ", cmd_repair, NULL},
    {"uncurse", NULL, cmd_uncurse, NULL},
    {"rest|R", NULL, cmd_rest, NULL},
    {"save", NULL, cmd_save, NULL},
    {"talk", NULL, cmd_talk, NULL},
    {"lore", "lore ", cmd_lore, NULL},
    {NULL, "fill ", cmd_fill, "fill <lantern>"},
    /*'pray' alone is the prayer below; 'pray <book>' is the cleric ritual.*/
    {"use|quaff|q|read|r|eat|e|zap|z|study|awaken|invoke|intone|celebrate|pronounce|"
     "practice|drill|rage|sneak|meditate",
     "use |quaff |q |read |r |eat |e |zap |z |study |pray |awaken |invoke |intone |"
     "celebrate |pronounce |practice |drill |rage |sneak |meditate ",
     cmd_use, NULL},
    {"spells", NULL, cmd_spells, NULL},
    {NULL, "cast |c ", cmd_cast, "cast <spell> [target]"},
    {"pickup", NULL, cmd_pickup, NULL},
    {"get|g", "get |g ", cmd_get, NULL},
    {NULL, "drop |d ", cmd_drop, "drop <n>"},
    {NULL, "throw |v ", cmd_throw, "throw <n> [direction]"},
    {"search|s", NULL, cmd_search, NULL},
    {"disarm|D", "disarm ", cmd_disarm, NULL},
    {"pray|p", NULL, cmd_pray, NULL},
    {"examine|ex", NULL, cmd_examine, NULL},
    {"tunnel|T", "tunnel |T |t ", cmd_tunnel, NULL},
    {"open|o", "open |o ", cmd_open, NULL},
    {"close|c", "close ", cmd_close, NULL}, /*"c <n>" is cast, above*/
};

const CmdEntry *cmd_commands(size_t *n) {
  *n = sizeof(COMMANDS) / sizeof(COMMANDS[0]);
  return COMMANDS;
}

void handle_text_cmd(Client *c, const char *cmd, NPC *npcs) {
  if (!c || !cmd)
    return;
  if (cmd[0] == '/')
    cmd++; /*"/look" works like "look"*/
  server_log("CMD", "%s: %s", c->username, cmd);
  size_t n = sizeof(COMMANDS) / sizeof(COMMANDS[0]);
  if (cmd_table_run(COMMANDS, n, c, cmd, npcs))
    return;
  /*A known command with missing or wrong arguments gets its usage.*/
  const CmdEntry *e = cmd_table_find_word(COMMANDS, n, cmd);
  const char *usage = e ? e->usage : (c->is_dm ? cmd_dm_usage(cmd) : NULL);
  if (usage) {
    send_text_to_client(c->sock, "[SYSTEM] Usage: %s", usage);
    return;
  }
  send_text_to_client(c->sock, "[SYSTEM] Unrecognized command '%s'. Type 'help' or '?' for the list.", cmd);
}
