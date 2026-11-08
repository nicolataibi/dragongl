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

static const CmdEntry COMMANDS[] = {
    {"help|?", NULL, cmd_help},
    {NULL, "train", cmd_train},
    {"top|leaderboard", NULL, cmd_top},
    {"event", NULL, cmd_event},
    {NULL, "invite ", cmd_invite},
    {"join", NULL, cmd_join},
    {"leave", NULL, cmd_leave},
    {"party", NULL, cmd_party},
    {NULL, "trade ", cmd_trade},
    {NULL, "offer ", cmd_offer},
    {"accept", NULL, cmd_accept},
    {"cancel", NULL, cmd_cancel},
    {NULL, "msg |tell |whisper ", cmd_msg},
    {NULL, "say |shout ", cmd_say},
    {"who", NULL, cmd_who},
    {NULL, "block ", cmd_block},
    {NULL, "unblock ", cmd_unblock},
    {"blocks|blocklist", NULL, cmd_blocks},
    {NULL, "browse ", cmd_browse},
    {NULL, NULL, cmd_dm},
    {"retrieve", NULL, cmd_retrieve},
    {"list", NULL, cmd_list},
    {"look|l|lock", "look |lock ", cmd_look},
    {"fountain", NULL, cmd_fountain},
    {"statue|crystal", NULL, cmd_statue},
    {"status|stats", NULL, cmd_stats},
    {"inventory|i", NULL, cmd_inventory},
    {NULL, "wear |equip |wield |w ", cmd_wear},
    {NULL, "remove |unequip |takeoff ", cmd_remove},
    {NULL, "buy ", cmd_buy},
    {NULL, "sell ", cmd_sell},
    {NULL, "haggle ", cmd_haggle},
    {NULL, "identify", cmd_identify},
    {"cure", NULL, cmd_cure},
    {NULL, "belt ", cmd_belt},
    {NULL, "unbelt", cmd_unbelt},
    {NULL, "repair", cmd_repair},
    {"uncurse", NULL, cmd_uncurse},
    {"rest|R", NULL, cmd_rest},
    {"save", NULL, cmd_save},
    {"talk", NULL, cmd_talk},
    {NULL, "lore", cmd_lore},
    {NULL, "fill ", cmd_fill},
    {"q|r|e|z|eat", "use|quaff|q |read|r |eat |e |zap|z |study|pray|awaken|invoke|intone|celebrate|pronounce|practice|drill|rage|sneak|meditate", cmd_use},
    {"spells", NULL, cmd_spells},
    {NULL, "cast |c ", cmd_cast},
    {"pickup", NULL, cmd_pickup},
    {"g", "get|g ", cmd_get},
    {NULL, "drop |d ", cmd_drop},
    {NULL, "throw |v ", cmd_throw},
    {"search|s", NULL, cmd_search},
    {"disarm|D", "disarm ", cmd_disarm},
    {"pray|p", NULL, cmd_pray},
    {"examine|ex", NULL, cmd_examine},
    {"tunnel|T", "tunnel |T |t ", cmd_tunnel},
    {"open|o", "open |o ", cmd_open},
    {"close|c", "close ", cmd_close}, /*"c <n>" is cast, above*/
};

void handle_text_cmd(Client *c, const char *cmd, NPC *npcs) {
  if (!c || !cmd)
    return;
  if (cmd[0] == '/')
    cmd++; /*"/look" works like "look"*/
  server_log("CMD", "%s: %s", c->username, cmd);
  if (cmd_table_run(COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0]), c, cmd, npcs))
    return;
  send_text_to_client(c->sock, "[SYSTEM] Unrecognized command '%s' or incorrect syntax. Type 'help' or '?' for the list.", cmd);
}
