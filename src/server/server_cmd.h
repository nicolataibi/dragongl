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
 * server_cmd.h - Internal interface of the text-command handlers.
 *
 * handle_text_cmd() (server_commands.c) walks a table of CmdEntry in order;
 * the first entry whose names or prefixes match the command runs its
 * handler, and stops the walk when the handler returns true. A handler that
 * returns false lets the walk go on, so the order of the table is part of
 * the command syntax (for example "c <n>" is cast, "c" alone is close).
 */
#ifndef SERVER_CMD_H
#define SERVER_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include "server_entities.h"

typedef bool (*CmdFn)(Client *c, const char *cmd, NPC *npcs);

typedef struct {
  const char *names;    /*"a|b": the command is exactly one of them, or NULL*/
  const char *prefixes; /*"a |b": the command starts with one of them, or NULL*/
  CmdFn run;            /*both NULL: every command reaches the handler*/
} CmdEntry;

/*Runs the first matching entry whose handler accepts the command; true if
 * one did.*/
bool cmd_table_run(const CmdEntry *table, size_t n, Client *c, const char *cmd, NPC *npcs);

/*Temples of the class rituals (cmd_world.c): look names them, use checks
 * that a ritual is performed inside the temple of the class.*/
typedef struct { int cls; int x0, x1, y0, y1; const char *name; const char *ritual; } TempleInfo;
extern const TempleInfo TEMPLES[];
extern const int TEMPLE_COUNT;

/* cmd_character.c */
bool cmd_help(Client *c, const char *cmd, NPC *npcs);
bool cmd_train(Client *c, const char *cmd, NPC *npcs);
bool cmd_stats(Client *c, const char *cmd, NPC *npcs);
bool cmd_rest(Client *c, const char *cmd, NPC *npcs);
bool cmd_save(Client *c, const char *cmd, NPC *npcs);

/* cmd_social.c */
bool cmd_top(Client *c, const char *cmd, NPC *npcs);
bool cmd_event(Client *c, const char *cmd, NPC *npcs);
bool cmd_invite(Client *c, const char *cmd, NPC *npcs);
bool cmd_join(Client *c, const char *cmd, NPC *npcs);
bool cmd_leave(Client *c, const char *cmd, NPC *npcs);
bool cmd_party(Client *c, const char *cmd, NPC *npcs);
bool cmd_trade(Client *c, const char *cmd, NPC *npcs);
bool cmd_offer(Client *c, const char *cmd, NPC *npcs);
bool cmd_accept(Client *c, const char *cmd, NPC *npcs);
bool cmd_cancel(Client *c, const char *cmd, NPC *npcs);
bool cmd_msg(Client *c, const char *cmd, NPC *npcs);
bool cmd_say(Client *c, const char *cmd, NPC *npcs);
bool cmd_who(Client *c, const char *cmd, NPC *npcs);
bool cmd_block(Client *c, const char *cmd, NPC *npcs);
bool cmd_unblock(Client *c, const char *cmd, NPC *npcs);
bool cmd_blocks(Client *c, const char *cmd, NPC *npcs);

/* cmd_shop.c */
bool cmd_browse(Client *c, const char *cmd, NPC *npcs);
bool cmd_list(Client *c, const char *cmd, NPC *npcs);
bool cmd_buy(Client *c, const char *cmd, NPC *npcs);
bool cmd_sell(Client *c, const char *cmd, NPC *npcs);
bool cmd_haggle(Client *c, const char *cmd, NPC *npcs);
bool cmd_identify(Client *c, const char *cmd, NPC *npcs);
bool cmd_cure(Client *c, const char *cmd, NPC *npcs);
bool cmd_repair(Client *c, const char *cmd, NPC *npcs);
bool cmd_uncurse(Client *c, const char *cmd, NPC *npcs);
bool cmd_talk(Client *c, const char *cmd, NPC *npcs);

/* cmd_items.c */
bool cmd_retrieve(Client *c, const char *cmd, NPC *npcs);
bool cmd_inventory(Client *c, const char *cmd, NPC *npcs);
bool cmd_wear(Client *c, const char *cmd, NPC *npcs);
bool cmd_remove(Client *c, const char *cmd, NPC *npcs);
bool cmd_belt(Client *c, const char *cmd, NPC *npcs);
bool cmd_unbelt(Client *c, const char *cmd, NPC *npcs);
bool cmd_lore(Client *c, const char *cmd, NPC *npcs);
bool cmd_fill(Client *c, const char *cmd, NPC *npcs);
bool cmd_use(Client *c, const char *cmd, NPC *npcs);
bool cmd_pickup(Client *c, const char *cmd, NPC *npcs);
bool cmd_get(Client *c, const char *cmd, NPC *npcs);
bool cmd_drop(Client *c, const char *cmd, NPC *npcs);
bool cmd_throw(Client *c, const char *cmd, NPC *npcs);

/* cmd_magic.c */
bool cmd_spells(Client *c, const char *cmd, NPC *npcs);
bool cmd_cast(Client *c, const char *cmd, NPC *npcs);
bool cmd_pray(Client *c, const char *cmd, NPC *npcs);

/* cmd_world.c */
bool cmd_look(Client *c, const char *cmd, NPC *npcs);
bool cmd_fountain(Client *c, const char *cmd, NPC *npcs);
bool cmd_statue(Client *c, const char *cmd, NPC *npcs);
bool cmd_search(Client *c, const char *cmd, NPC *npcs);
bool cmd_disarm(Client *c, const char *cmd, NPC *npcs);
bool cmd_examine(Client *c, const char *cmd, NPC *npcs);
bool cmd_tunnel(Client *c, const char *cmd, NPC *npcs);
bool cmd_open(Client *c, const char *cmd, NPC *npcs);
bool cmd_close(Client *c, const char *cmd, NPC *npcs);

/* cmd_dm.c */
bool cmd_dm(Client *c, const char *cmd, NPC *npcs);
bool cmd_dm(Client *c, const char *cmd, NPC *npcs);

#endif /* SERVER_CMD_H */
