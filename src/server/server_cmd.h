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
 * When no handler accepts a line, handle_text_cmd() answers with the usage
 * of the command named by its first word, or says it is not a command.
 */
#ifndef SERVER_CMD_H
#define SERVER_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include "server_entities.h"

typedef bool (*CmdFn)(Client *c, const char *cmd, Game *game);

typedef struct {
    const char *names;    /*"a|b": the command is exactly one of them, or NULL*/
    const char *prefixes; /*"a |b": the command starts with one of them, or NULL;
                         * every prefix ends with a space, so a prefix never
                         * matches a longer word ("train " is not "trainer")*/
    CmdFn run;            /*names and prefixes NULL: every command reaches it*/
    const char *usage;    /*shown when the first word is this command but no
                         * handler accepted the line, or NULL*/
} CmdEntry;

/*Runs the first matching entry whose handler accepts the command; true if
 * one did.*/
bool cmd_table_run(const CmdEntry *table, size_t n, Client *c, const char *cmd, Game *game);

/*The first entry of the table whose command word is the first word of cmd
 * (a name, or a prefix without its space), or NULL.*/
const CmdEntry *cmd_table_find_word(const CmdEntry *table, size_t n, const char *cmd);

/*The player command table (server_commands.c), for the tests.*/
const CmdEntry *cmd_commands(size_t *n);

/*The DM command table (cmd_dm.c), for the tests.*/
const CmdEntry *cmd_dm_commands(size_t *n);

/*The usage line of a dm_* command (cmd_dm.c), or NULL.*/
const char *cmd_dm_usage(const char *cmd);

/*The topics of `help` and of docs/COMMANDS.md, in their order.*/
typedef enum {
    CMD_GROUP_WORLD,
    CMD_GROUP_CHARACTER,
    CMD_GROUP_ITEMS,
    CMD_GROUP_MAGIC,
    CMD_GROUP_TOWN,
    CMD_GROUP_TRADE,
    CMD_GROUP_SOCIAL,
    CMD_GROUP_DM, /*shown to the dm account only*/
    CMD_GROUP_COUNT
} CmdGroup;

/*What a command is for (cmd_help.c). Every word the command tables accept
 * is the name or an alias of one entry: the tests check it.*/
typedef struct {
    const char *name;    /*the word to remember*/
    const char *aliases; /*"a|b": the other words that do the same, or NULL*/
    CmdGroup group;
    const char *synopsis; /*the name with its arguments*/
    const char *help;     /*one sentence*/
} CmdDoc;

const CmdDoc *cmd_docs(size_t *n);
const CmdDoc *cmd_doc_find(const char *word);
const char *cmd_group_topic(CmdGroup group);

/*Temples of the class rituals (cmd_world.c): look names them, use checks
 * that a ritual is performed inside the temple of the class.*/
typedef struct {
    int cls;
    int x0, x1, y0, y1;
    const char *name;
    const char *ritual;
} TempleInfo;
extern const TempleInfo TEMPLES[];
extern const int TEMPLE_COUNT;

/* cmd_help.c */
bool cmd_help(Client *c, const char *cmd, Game *game);

/* cmd_character.c */
bool cmd_train(Client *c, const char *cmd, Game *game);
bool cmd_stats(Client *c, const char *cmd, Game *game);
bool cmd_rest(Client *c, const char *cmd, Game *game);
bool cmd_save(Client *c, const char *cmd, Game *game);

/* cmd_social.c */
bool cmd_top(Client *c, const char *cmd, Game *game);
bool cmd_event(Client *c, const char *cmd, Game *game);
bool cmd_invite(Client *c, const char *cmd, Game *game);
bool cmd_join(Client *c, const char *cmd, Game *game);
bool cmd_leave(Client *c, const char *cmd, Game *game);
bool cmd_party(Client *c, const char *cmd, Game *game);
bool cmd_trade(Client *c, const char *cmd, Game *game);
bool cmd_offer(Client *c, const char *cmd, Game *game);
bool cmd_accept(Client *c, const char *cmd, Game *game);
bool cmd_cancel(Client *c, const char *cmd, Game *game);
bool cmd_msg(Client *c, const char *cmd, Game *game);
bool cmd_say(Client *c, const char *cmd, Game *game);
bool cmd_who(Client *c, const char *cmd, Game *game);
bool cmd_block(Client *c, const char *cmd, Game *game);
bool cmd_unblock(Client *c, const char *cmd, Game *game);
bool cmd_blocks(Client *c, const char *cmd, Game *game);

/* cmd_shop.c */
bool cmd_browse(Client *c, const char *cmd, Game *game);
bool cmd_list(Client *c, const char *cmd, Game *game);
bool cmd_buy(Client *c, const char *cmd, Game *game);
bool cmd_sell(Client *c, const char *cmd, Game *game);
bool cmd_haggle(Client *c, const char *cmd, Game *game);
bool cmd_identify(Client *c, const char *cmd, Game *game);
bool cmd_cure(Client *c, const char *cmd, Game *game);
bool cmd_repair(Client *c, const char *cmd, Game *game);
bool cmd_uncurse(Client *c, const char *cmd, Game *game);
bool cmd_talk(Client *c, const char *cmd, Game *game);

/* cmd_items.c */
bool cmd_retrieve(Client *c, const char *cmd, Game *game);
bool cmd_inventory(Client *c, const char *cmd, Game *game);
bool cmd_wear(Client *c, const char *cmd, Game *game);
bool cmd_remove(Client *c, const char *cmd, Game *game);
bool cmd_belt(Client *c, const char *cmd, Game *game);
bool cmd_unbelt(Client *c, const char *cmd, Game *game);
bool cmd_lore(Client *c, const char *cmd, Game *game);
bool cmd_fill(Client *c, const char *cmd, Game *game);
bool cmd_use(Client *c, const char *cmd, Game *game);
bool cmd_pickup(Client *c, const char *cmd, Game *game);
bool cmd_get(Client *c, const char *cmd, Game *game);
bool cmd_drop(Client *c, const char *cmd, Game *game);
bool cmd_throw(Client *c, const char *cmd, Game *game);

/* cmd_magic.c */
bool cmd_spells(Client *c, const char *cmd, Game *game);
bool cmd_cast(Client *c, const char *cmd, Game *game);
bool cmd_pray(Client *c, const char *cmd, Game *game);

/* cmd_world.c */
bool cmd_look(Client *c, const char *cmd, Game *game);
bool cmd_fountain(Client *c, const char *cmd, Game *game);
bool cmd_statue(Client *c, const char *cmd, Game *game);
bool cmd_search(Client *c, const char *cmd, Game *game);
bool cmd_disarm(Client *c, const char *cmd, Game *game);
bool cmd_examine(Client *c, const char *cmd, Game *game);
bool cmd_tunnel(Client *c, const char *cmd, Game *game);
bool cmd_open(Client *c, const char *cmd, Game *game);
bool cmd_close(Client *c, const char *cmd, Game *game);

/* cmd_dm.c */
bool cmd_dm(Client *c, const char *cmd, Game *game);

#endif /* SERVER_CMD_H */
