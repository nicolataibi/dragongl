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
 * spell_router.h — Magic Router: Dispatcher for special spells
 *
 * Architecture:
 *   - A static table of SpellHook entries (name -> function) is initialized
 *     only once at startup by spell_router_init().
 *   - The spell_router_dispatch() dispatcher performs an O(log N) binary
 *     search in the table to find a custom handler for the current spell.
 *   - If found, the custom handler is executed and the function returns true,
 *     signaling to the caller that the spell has been handled.
 *   - If not found, it returns false and the generic logic takes over.
 *
 * To add a new special spell:
 *   1. Write the handler function in spell_router.c (signature: SpellHandlerFn).
 *   2. Add an entry {name, handler} to the g_spell_hooks[] array.
 *   3. The table is automatically sorted at runtime by spell_router_init().
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
    Client       *caster;      /*The client casting the spell*/
    SpellTemplate *sp;         /*Current spell template*/
    NPC          *npcs;        /*Global array of NPCs*/
    NPC          *target;      /* Single target found (can be NULL) */
} SpellContext;

/*--------------------------------------------------------------------------
 * Type of handler function pointer.
 * Returns true if the spell was fully handled by the hook.
 * ----------------------------------------------------------------------------*/
typedef bool (*SpellHandlerFn)(SpellContext *ctx);

/*--------------------------------------------------------------------------
 * Structure of a single entry in the lookup table.
 * ----------------------------------------------------------------------------*/
typedef struct {
    const char   *name;        /*Exact spell name (case-insensitive)*/
    SpellHandlerFn handler;    /*Custom management function*/
} SpellHook;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * spell_router_init - Sort hook table by binary search.
 * Must only be called once after loading the spell database.*/
void spell_router_init(void);

/**
 * spell_router_dispatch - Search and invoke the custom handler for the spell.
 *
 * @ctx: Context of the current spell.
 * Returns true if a handler handled the spell (skip generic logic).
 * Returns false if no hook exists (uses standard generic logic).*/
bool spell_router_dispatch(SpellContext *ctx);

#endif /* SPELL_ROUTER_H */
