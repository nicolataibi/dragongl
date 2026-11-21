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
 * server_conquest.c - The end of the game (see server_conquest.h).
 */

#include "server_conquest.h"
#include "player.h"
#include "server_internal.h"
#include "server_log.h"
#include "server_world.h"

#include <stdio.h>
#include <time.h>

/*The epilogue of LORE.md (Epilogue: The Light Beyond Darkness), told to
 * the one who conquered.*/
static const char *const k_epilogue[] = {
    "FLOOR 100 COMPLETED - DRAGON'S MAW CONQUERED",
    "The core of the labyrinth, purified of its primordial evil, casts a beam of white "
    "light upward.",
    "The walls of rock and ash open, and for the first time in centuries the true starry "
    "sky shines above the dungeon.",
    "You entered on Floor 0 as a mere mercenary. You rise toward the surface as one of the "
    "Immortal Legends of Dragon's Maw.",
    "Your legend is written: the town welcomes you, the dungeon is closed to you, and "
    "nothing you do from now on is saved.",
};

void conquest_date(int64_t at, char *out, size_t cap) {
    time_t t = (time_t)at;
    struct tm tm;
    if (at <= 0 || !gmtime_r(&t, &tm) || strftime(out, cap, "%Y-%m-%d", &tm) == 0) {
        snprintf(out, cap, "?");
    }
}

void conquest_refuse(const Client *c) {
    send_text_to_client(c->sock, "[CONQUEST] Your legend is written: the dungeon and its "
                                 "riches are closed to you, and nothing more is saved.");
}

void conquest_complete(Game *game, Client *c) {
    c->conquered_at = (int64_t)time(NULL);
    send_text_to_client(c->sock, "[CONQUEST] You have conquered the Dragon's Maw!");
    for (size_t i = 0; i < sizeof(k_epilogue) / sizeof(k_epilogue[0]); i++) {
        send_text_to_client(c->sock, "[EPILOGUE] %s", k_epilogue[i]);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client *p = &game->clients[i];
        if (p != c && p->active && p->authenticated) {
            send_text_to_client(p->sock, "[CONQUEST] %s has conquered the dungeon!", c->username);
        }
    }
    server_log("CONQUEST", "%s conquered the dungeon (level %d).", c->username, c->level);
    player_respawn_town(game, c);
    /*The last save: from now on the character is a legend, kept as it was.*/
    save_player_data(c);
    c->saves_sealed = true;
}
