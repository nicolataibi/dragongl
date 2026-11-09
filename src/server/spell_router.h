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
 * spell_router.h — Magic Router: the handlers of the special spells
 *
 * A spell of kind "special" names its handler in data/spells.json
 * ("handler": "teleport"); spell_router_dispatch() calls it. Adding a
 * handler: a SpellHandler value (spells.h), its name (spell_effect.c), the
 * function and its slot in g_handlers[] (spell_router.c), and the name in
 * the schema enum (data/schema/spells.schema.json).
 */

#ifndef SPELL_ROUTER_H
#define SPELL_ROUTER_H

#include <stdbool.h>
#include "server_entities.h"
#include "spells.h"

/*--------------------------------------------------------------------------
 * Context passed to each custom handler.
 * All pointers are guaranteed non-NULL at call time.
 * ----------------------------------------------------------------------------*/
typedef struct {
    Client *caster;          /*The client casting the spell*/
    const SpellTemplate *sp; /*Current spell template*/
    NPC *npcs;               /*Global array of NPCs*/
    NPC *target;             /* Single target found (can be NULL) */
} SpellContext;

/*--------------------------------------------------------------------------
 * Type of handler function pointer.
 * Returns true if the spell was fully handled by the hook.
 * ----------------------------------------------------------------------------*/
typedef bool (*SpellHandlerFn)(SpellContext *ctx);

/*Logs the number of handlers; called once after loading the spells.*/
void spell_router_init(void);

/*Calls the handler of ctx->sp; false if the spell has none.*/
bool spell_router_dispatch(SpellContext *ctx);

/*Stamps the spell's terrain (wall, ice, mud) around (x, y), radius the
 * effect's radius (2 if it has none). The caster's cell stays free.*/
void spell_router_terrain(Client *c, const SpellTemplate *sp, int x, int y);

#endif /* SPELL_ROUTER_H */
