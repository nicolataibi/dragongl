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

/**
 * server_commands.h — Public interface of the game commands module
 *
 * Player and GM text command management function declaration.*/
#ifndef SERVER_COMMANDS_H
#define SERVER_COMMANDS_H

#include "server_entities.h"

/**
 * handle_text_cmd - Process a text command (look, buy, cast, dm_*, etc.)
 * @c: The client sending the command.
 * @cmd: The text command.
 * @npcs: The array of all NPCs on the server.*/
void handle_text_cmd(Client *c, const char *cmd, NPC *npcs);

#endif // SERVER_COMMANDS_H
