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
 * spell_router.c — Magic Router: Implement special handlers
 *
 * Each handle_spell_* function implements the custom logic of a spell
 * or a category of spells that does not reduce to "general damage" or
 * "generic buff".
 *
 * A spell reaches its handler through the "handler" field of its effect
 * (data/spells.json), never through its name.
 *
 * Categories implemented:
 * 1. TERRAIN — Edit voxels on the map (the "terrain" field of an effect)
 * 2. SUMMON — Summons a new NPC ally next to the caster
 * 3. CHARM — Change the faction of a target NPC
 * 4. UTILITY — Teleport, Invisibility and other special effects*/

#include "spell_router.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "server_world.h"
#include "pathfinding.h"
#include "map.h"
#include "rules.h"
#include "bestiary.h"
#include "spell_cast.h"
#include "spell_rules.h"
#include "player.h"
#include "server_log.h"
#include "server_tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdbool.h>

/* =========================================================================
 * Internal prototypes
 * ========================================================================= */
static bool handle_summon_elemental(SpellContext *ctx);
static bool handle_animate_dead(SpellContext *ctx);
static bool handle_charm_person(SpellContext *ctx);
static bool handle_dominate_monster(SpellContext *ctx);
static bool handle_teleport(SpellContext *ctx);
static bool handle_invisibility(SpellContext *ctx);
static bool handle_blink(SpellContext *ctx);
static bool handle_class_transit(SpellContext *ctx);

/*One handler per SpellHandler value (spell_effect.c names them).*/
static const SpellHandlerFn g_handlers[SPELL_HANDLER_COUNT] = {
    [SPELL_HANDLER_ANIMATE_DEAD] = handle_animate_dead,
    [SPELL_HANDLER_BLINK] = handle_blink,
    [SPELL_HANDLER_CHARM_PERSON] = handle_charm_person,
    [SPELL_HANDLER_CLASS_TRANSIT] = handle_class_transit,
    [SPELL_HANDLER_DOMINATE_MONSTER] = handle_dominate_monster,
    [SPELL_HANDLER_INVISIBILITY] = handle_invisibility,
    [SPELL_HANDLER_SUMMON_ELEMENTAL] = handle_summon_elemental,
    [SPELL_HANDLER_TELEPORT] = handle_teleport,
};

void spell_router_init(void) {
    int count = 0;
    for (int h = SPELL_HANDLER_NONE + 1; h < SPELL_HANDLER_COUNT; h++) {
        count += g_handlers[h] != NULL;
    }
    server_log("MAGIC", "Magic Router initialized: %d handlers.", count);
}

bool spell_router_dispatch(SpellContext *ctx) {
    if (!ctx || !ctx->sp) {
        return false;
    }
    SpellHandler h = ctx->sp->effect.handler;
    if (h <= SPELL_HANDLER_NONE || h >= SPELL_HANDLER_COUNT || !g_handlers[h]) {
        return false;
    }
    return g_handlers[h](ctx);
}

/* =========================================================================
 * ─────────────────────────────────────────────────────────────────────────
 *  CATEGORY 1: TERRAIN — Voxel Editing
 * ─────────────────────────────────────────────────────────────────────────
 * ========================================================================= */

/**
 * spell_terrain_stamp — Generic function for printing a voxel over an area
 * Circulate around the target.
 *
 * @floor: Floor on which to edit the map.
 * @cx, cy: Center of the area of ​​effect.
 * @radius: Radius (Manhattan cells).
 * @new_type: Voxel type to apply.
 * @only_floor: If true, replaces VOXEL_FLOOR only (not walls or rocks).
 * Returns the number of cells modified.*/
static int spell_terrain_stamp(Floor *floor, int cx, int cy, int radius, VoxelType new_type,
                               bool only_floor) {
    int changed = 0;
    int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }

            int nx = cx + dx;
            int ny = cy + dy;

            if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
                continue;
            }

            VoxelType current = floor->map.data[0][ny][nx];

            if (only_floor && current != VOXEL_FLOOR) {
                continue;
            }

            floor->map.data[0][ny][nx] = new_type;
            changed++;
        }
    }

    return changed;
}

void spell_router_terrain(Game *game, Client *c, const SpellTemplate *sp, int x, int y) {
    static const struct {
        VoxelType voxel;
        const char *what;
    } k_terrain[] = {
        [SPELL_TERRAIN_WALL] = {VOXEL_WALL, "solidify into rough stone"},
        [SPELL_TERRAIN_ICE] = {VOXEL_ICE, "are covered in slippery ice"},
        [SPELL_TERRAIN_MUD] = {VOXEL_MUD, "are swallowed by the mud"},
    };
    SpellTerrain t = sp->effect.terrain;
    if (t <= SPELL_TERRAIN_NONE || t > SPELL_TERRAIN_MUD || !game->world || c->floor_id < 0 ||
        c->floor_id >= MAX_FLOORS) {
        return;
    }
    Floor *floor = &game->world->floors[c->floor_id];
    int radius = sp->effect.radius > 0 ? sp->effect.radius : 2;
    int changed = spell_terrain_stamp(floor, x, y, radius, k_terrain[t].voxel, true);
    /*The caster's cell always stays passable.*/
    floor->map.data[0][c->y][c->x] = VOXEL_FLOOR;
    send_text_to_client(c->sock, "  > %s: %d cells %s.", sp->name, changed, k_terrain[t].what);
    server_log("MAGIC", "%s: %s at (%d,%d) floor %d [radius %d, %d cells].", c->username, sp->name,
               x, y, c->floor_id, radius, changed);
}

/*============================================================================
 * ──────────────────────────────────── ─────────────────────────────────────
 * CATEGORY 2: SUMMON — Summon Allied NPCs
 * ──────────────────────────────────── ─────────────────────────────────────
 *
 * Summoned NPCs are identified by the custom_name field with prefix
 * "[SUMMONED]". The AIContext.current_target_id is set to the caster for
 * indicates the "allied faction" and the Behavior Tree decides aggression
 * towards the player's enemies.
 * ==========================================================================*/

/**
 * find_spawn_cell — Searches for a free VOXEL_FLOOR cell adjacent to the caster.
 *Try the 8 cardinal + diagonal directions, then expand the radius.
 * Returns true and sets *out_x, *out_y if found.*/
static bool find_spawn_cell(Game *game, Client *c, int *out_x, int *out_y) {
    Floor *floor = &game->world->floors[c->floor_id];

    static const int dx[] = {0, 1, 0, -1, 1, -1, 1, -1};
    static const int dy[] = {-1, 0, 1, 0, -1, 1, 1, -1};

    for (int i = 0; i < 8; i++) {
        int nx = c->x + dx[i];
        int ny = c->y + dy[i];

        if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
            continue;
        }

        if (floor->map.data[0][ny][nx] == VOXEL_FLOOR && floor->entity_grid[ny][nx] == 0) {
            *out_x = nx;
            *out_y = ny;
            return true;
        }
    }

    /*Second attempt: radius 2*/
    for (int dy2 = -2; dy2 <= 2; dy2++) {
        for (int dx2 = -2; dx2 <= 2; dx2++) {
            if (abs(dx2) < 2 && abs(dy2) < 2) {
                continue; /* already checked */
            }

            int nx = c->x + dx2;
            int ny = c->y + dy2;

            if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
                continue;
            }

            if (floor->map.data[0][ny][nx] == VOXEL_FLOOR && floor->entity_grid[ny][nx] == 0) {
                *out_x = nx;
                *out_y = ny;
                return true;
            }
        }
    }

    return false;
}

/*-----------------------------------------------------------------------
 * Summon Elemental — Summons an elemental next to the caster.
 * Chooses type based on surrounding terrain (fire/water/earth/air).
 * -------------------------------------------------------------------------*/
static bool handle_summon_elemental(SpellContext *ctx) {
    Client *c = ctx->caster;
    Game *game = ctx->game;

    NPC *slot = npc_alloc(game);
    if (!slot) {
        send_text_to_client(c->sock,
                            "[MAGIC] The ethereal plane is overcrowded: summoning failed!");
        return true;
    }

    int sx = 0;
    int sy = 0;

    if (!find_spawn_cell(game, c, &sx, &sy)) {
        send_text_to_client(c->sock, "[MAGIC] No free space to summon the elemental!");
        return true;
    }

    /*Choose the elemental type based on the voxel under the caster*/
    Floor *floor = &game->world->floors[c->floor_id];
    VoxelType under = floor->map.data[0][c->y][c->x];

    const char *elem_name = "Earth Elemental";
    int elem_hp = 40;
    int elem_atk = 5;
    int elem_dmg = 6;
    int elem_ac = 13;

    if (under == VOXEL_LAVA || under == VOXEL_ASH) {
        elem_name = "Fire Elemental";
        elem_hp = 52;
        elem_atk = 7;
        elem_dmg = 8;
        elem_ac = 12;
    } else if (under == VOXEL_WATER || under == VOXEL_ICE) {
        elem_name = "Water Elemental";
        elem_hp = 46;
        elem_atk = 6;
        elem_dmg = 6;
        elem_ac = 14;
    } else if (under == VOXEL_GRASS || under == VOXEL_FLOOR) {
        elem_name = "Air Elemental";
        elem_hp = 38;
        elem_atk = 8;
        elem_dmg = 6;
        elem_ac = 15;
    }

    /* Initialize the summoned NPC */
    memset(slot, 0, sizeof(NPC));
    slot->entity_id = game->next_id++;
    /*Through npc_set_active: the slot was just zeroed (template==NULL), so
     * the floor counters skip it, like floor_stats_rebuild does.*/
    npc_set_active(game, slot, true);
    slot->archetype = ARCH_MELEE;
    slot->template = NULL;
    slot->template_idx = -1;
    slot->x = sx;
    slot->y = sy;
    slot->floor_id = c->floor_id;
    slot->hp = elem_hp;
    slot->max_hp = elem_hp;
    slot->ac = elem_ac;
    slot->attack_bonus = elem_atk;
    slot->damage_dice = elem_dmg;
    slot->damage_sides = 1; /* fixed damage: flat elem_dmg */
    slot->xp_reward = 0;    /*Summoning yields no XP*/
    slot->gold_drop = 0;
    slot->morale = 100;
    slot->energy = 0;
    slot->spawn_x = sx;
    slot->spawn_y = sy;
    slot->respawn_timer = 0;

    /*Ally Marker: The target_id points to the caster's entity_id*/
    slot->ai_ctx.current_target_id = c->entity_id;

    snprintf(slot->custom_name, sizeof(slot->custom_name), "[SUMMONED] %s", elem_name);

    /* Update the entity grid */
    floor->entity_grid[sy][sx] = slot->entity_id;

    send_text_to_client(c->sock, "[MAGIC] %s: a %s materializes from the ethereal plane!",
                        ctx->sp->name, elem_name);
    send_text_to_client(c->sock, "  > The entity (HP:%d AC:%d) will follow you into battle.",
                        elem_hp, elem_ac);

    server_log("MAGIC", "%s summons %s (ID:%d) at (%d,%d) floor %d.", c->username, elem_name,
               slot->entity_id, sx, sy, c->floor_id);

    return true;
}

/*-----------------------------------------------------------------------
 * Animate Dead — Summons a skeleton from the remains of a dead NPC.
 * Finds the nearest inactive NPC on the same floor.
 * -------------------------------------------------------------------------*/
static bool handle_animate_dead(SpellContext *ctx) {
    Client *c = ctx->caster;
    Game *game = ctx->game;

    /*Search for a nearby corpse (NPC not active with valid template)*/
    NPC *corpse = NULL;
    int min_d = 8; /*maximum summon range*/

    for (int i = 0; i < MAX_NPCS; i++) {
        if (game->npcs[i].active) {
            continue;
        }
        if (game->npcs[i].template == NULL) {
            continue;
        }
        if (game->npcs[i].floor_id != c->floor_id) {
            continue;
        }

        int d = abs(game->npcs[i].x - c->x) + abs(game->npcs[i].y - c->y);
        if (d < min_d) {
            min_d = d;
            corpse = &game->npcs[i];
        }
    }

    if (!corpse) {
        send_text_to_client(c->sock,
                            "[MAGIC] Animate Dead: There are no corpses nearby to reanimate.");
        return true;
    }

    /*Reactivate the corpse as an undead ally (it has a template, so
     * npc_set_active also counts it on the floor)*/
    npc_set_active(game, corpse, true);
    corpse->hp = corpse->max_hp / 2; /* revives with half HP */
    corpse->xp_reward = 0;
    corpse->gold_drop = 0;
    corpse->ai_ctx.current_target_id = c->entity_id; /* allied faction */

    const char *original = corpse->template ? corpse->template->name : "Creature";
    snprintf(corpse->custom_name, sizeof(corpse->custom_name), "[SUMMONED] Skeleton of %s",
             original);

    Floor *floor = &game->world->floors[c->floor_id];
    floor->entity_grid[corpse->y][corpse->x] = corpse->entity_id;

    send_text_to_client(c->sock, "[MAGIC] Animate Dead: The bones of the %s rise at your command!",
                        original);
    send_text_to_client(c->sock, "  > The skeleton (HP:%d) is ready to fight for you.", corpse->hp);

    return true;
}

/* =========================================================================
 * ─────────────────────────────────────────────────────────────────────────
 *  CATEGORY 3: CHARM / MENTAL CONTROL
 * ─────────────────────────────────────────────────────────────────────────
 * ========================================================================= */

/*The target's saving throw (the effect's save, normally Wisdom) against
 * the caster's spell DC; true if it resists. *dc receives the DC.*/
static bool mind_save(const SpellContext *ctx, int *dc) {
    SpellCaster sc;
    spell_caster_of(ctx->caster, &sc);
    *dc = spell_save_dc(&ctx->sp->effect, &sc);
    return rules_roll_save(npc_save_bonus(ctx->target), *dc, false, false, NULL);
}

static void mind_effect(NPC *tgt, const char *name, int rounds) {
    ActiveEffect fx;
    memset(&fx, 0, sizeof(fx));
    fx.name = name;
    fx.trigger = EVENT_ON_TURN_START;
    fx.duration_rounds = rounds;
    rules_add_effect(tgt->effects, &tgt->effect_count, MAX_EFFECTS_PER_ENTITY, &fx);
}

/*-----------------------------------------------------------------------
 * Charm Person — the target stops attacking the caster for the effect's
 * rounds: its AI target is cleared and it takes the Charmed condition.
 * -------------------------------------------------------------------------*/
static bool handle_charm_person(SpellContext *ctx) {
    Client *c = ctx->caster;
    NPC *tgt = ctx->target;
    if (!tgt) {
        send_text_to_client(c->sock, "[MAGIC] %s: no target in range.", ctx->sp->name);
        return true;
    }
    int dc = 0;
    if (mind_save(ctx, &dc)) {
        send_text_to_client(c->sock, "[MAGIC] %s resists your charm! (save vs DC %d)",
                            npc_name(tgt), dc);
        return true;
    }
    tgt->ai_ctx.current_target_id = -1;
    mind_effect(tgt, condition_to_name(COND_CHARMED), ctx->sp->effect.rounds);
    send_text_to_client(c->sock, "[MAGIC] %s: %s gazes at you dreamily... charmed for %d rounds.",
                        ctx->sp->name, npc_name(tgt), ctx->sp->effect.rounds);
    server_log("MAGIC", "%s charms %s (ID:%d) with %s.", c->username, npc_name(tgt), tgt->entity_id,
               ctx->sp->name);
    return true;
}

/*-----------------------------------------------------------------------
 * Dominate Monster — the target fights for the caster for the effect's
 * rounds.
 * -------------------------------------------------------------------------*/
static bool handle_dominate_monster(SpellContext *ctx) {
    Client *c = ctx->caster;
    NPC *tgt = ctx->target;
    if (!tgt) {
        send_text_to_client(c->sock, "[MAGIC] %s: no target in range.", ctx->sp->name);
        return true;
    }
    int dc = 0;
    if (mind_save(ctx, &dc)) {
        send_text_to_client(c->sock, "[MAGIC] %s resists domination! (save vs DC %d)",
                            npc_name(tgt), dc);
        return true;
    }
    tgt->ai_ctx.current_target_id = c->entity_id; /* allied faction */
    mind_effect(tgt, "Dominated", ctx->sp->effect.rounds);
    snprintf(tgt->custom_name, sizeof(tgt->custom_name), "[DOMINATED] %s",
             tgt->template ? tgt->template->name : "Creature");
    send_text_to_client(c->sock, "[MAGIC] %s: %s's mind yields to your will for %d rounds!",
                        ctx->sp->name, tgt->template ? tgt->template->name : "the creature",
                        ctx->sp->effect.rounds);
    return true;
}

/* =========================================================================
 * ─────────────────────────────────────────────────────────────────────────
 *  CATEGORY 4: UTILITY — Teleport, Invisibility, Blink
 * ─────────────────────────────────────────────────────────────────────────
 * ========================================================================= */

/*-----------------------------------------------------------------------
 * Teleport — Instantly moves the caster to a random position
 * of the current floor that is VOXEL_FLOOR and unoccupied.
 * For now implemented as "Teleport random" (basic version).
 * -------------------------------------------------------------------------*/
static bool handle_teleport(SpellContext *ctx) {
    Game *game = ctx->game;
    Client *c = ctx->caster;
    Floor *floor = &game->world->floors[c->floor_id];

    /* Collect available free cells */
    int free_xs[256];
    int free_ys[256];
    int free_count = 0;

    /*Sparse sampling for performance (checks 1/16 of cells)*/
    for (int y = 2; y < MAP_HEIGHT - 2 && free_count < 256; y += 4) {
        for (int x = 2; x < MAP_WIDTH - 2 && free_count < 256; x += 4) {
            if (floor->map.data[0][y][x] == VOXEL_FLOOR && floor->entity_grid[y][x] == 0) {
                free_xs[free_count] = x;
                free_ys[free_count] = y;
                free_count++;
            }
        }
    }

    if (free_count == 0) {
        send_text_to_client(c->sock,
                            "[MAGIC] Teleport: the spatial fabric is unstable, teleport failed!");
        return true;
    }

    /*Remove from old location*/
    floor->entity_grid[c->y][c->x] = 0;

    /*Choose a random destination*/
    int dest = rules_roll_dice(1, free_count) - 1;
    int old_x = c->x;
    int old_y = c->y;

    c->x = free_xs[dest];
    c->y = free_ys[dest];

    /*Update the grid*/
    floor->entity_grid[c->y][c->x] = c->entity_id;

    send_text_to_client(c->sock, "[MAGIC] Teleport: a rip in the ether sucks you in...");
    send_text_to_client(c->sock, "> You materialize in (%d,%d). [Previous: (%d,%d)]", c->x, c->y,
                        old_x, old_y);

    server_log("MAGIC", "%s teleports from (%d,%d) to (%d,%d) [floor %d].", c->username, old_x,
               old_y, c->x, c->y, c->floor_id);

    return true;
}

/*-----------------------------------------------------------------------
 * Invisibility — Makes the caster invisible for N rounds.
 * Mechanic: Adds "Invisible" status to client effects.
 * The AI ​​in ai.c ignores targets with this status in target calculation.
 * -------------------------------------------------------------------------*/
static bool handle_invisibility(SpellContext *ctx) {
    Client *c = ctx->caster;
    const SpellTemplate *sp = ctx->sp;

    if (c->effect_count >= MAX_EFFECTS_PER_ENTITY) {
        send_text_to_client(c->sock,
                            "[MAGIC] You cannot sustain other magical effects at the same time.");
        return true;
    }

    /* Check if already invisible */
    for (int i = 0; i < c->effect_count; i++) {
        if (strcmp(c->effects[i].name, condition_to_name(COND_INVISIBLE)) == 0) {
            send_text_to_client(c->sock, "[MAGIC] You are already invisible (%d rounds remaining).",
                                c->effects[i].duration_rounds);
            return true;
        }
    }

    ActiveEffect invis_fx;
    memset(&invis_fx, 0, sizeof(invis_fx));
    invis_fx.name = condition_to_name(COND_INVISIBLE);
    invis_fx.duration_rounds = sp->effect.rounds > 0 ? sp->effect.rounds : 5;
    c->effects[c->effect_count++] = invis_fx;

    send_text_to_client(c->sock, "[MAGIC] %s: Your body dissolves into the air!", sp->name);
    send_text_to_client(
        c->sock,
        "  > You are invisible for %d rounds. Attacking or casting spells will end the effect.",
        invis_fx.duration_rounds);

    return true;
}

/*-----------------------------------------------------------------------
 * Blink — Unpredictable short teleport (range 5-10 cells).
 * Mechanics: Move the caster to a free cell at a distance of 5-10 in one
 *random direction. Safe: it doesn't end up inside the walls.
 * -------------------------------------------------------------------------*/
static bool handle_blink(SpellContext *ctx) {
    Game *game = ctx->game;
    Client *c = ctx->caster;
    Floor *floor = &game->world->floors[c->floor_id];

    int blink_dist = 5 + rules_roll_dice(1, 6); /* 5-10 cells */

    /*Test up to 16 discrete angles (22.5° steps)*/
    int landed_x = -1;
    int landed_y = -1;

    for (int attempt = 0; attempt < 16 && landed_x < 0; attempt++) {
        double angle = (attempt * 22.5) * (3.14159265 / 180.0);
        int dx = (int)round(cos(angle) * blink_dist);
        int dy = (int)round(sin(angle) * blink_dist);

        int nx = c->x + dx;
        int ny = c->y + dy;

        if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
            continue;
        }

        if (floor->map.data[0][ny][nx] == VOXEL_FLOOR && floor->entity_grid[ny][nx] == 0) {
            landed_x = nx;
            landed_y = ny;
        }
    }

    if (landed_x < 0) {
        send_text_to_client(c->sock,
                            "[MAGIC] Blink: the ethereal plane repels you, no free space!");
        return true;
    }

    floor->entity_grid[c->y][c->x] = 0;
    int old_x = c->x;
    int old_y = c->y;

    c->x = landed_x;
    c->y = landed_y;

    floor->entity_grid[c->y][c->x] = c->entity_id;

    send_text_to_client(
        c->sock,
        "[MAGIC] Blink: a flash of light and you vanish... only to reappear %d cells away!",
        blink_dist);
    send_text_to_client(c->sock, "> From (%d,%d) to (%d,%d).", old_x, old_y, c->x, c->y);

    return true;
}

/*============================================================================
 * CATEGORY 5: TRANSIT — Innate class travel (12 cantrips, one per class)
 *
 * All 12 spells share the same mechanic, wrapped in class-specific lore:
 *   - on the surface (floor 0): teleport to the deepest floor the character
 *     has ever explored (c->max_floor_explored);
 *   - underground (floor > 0): teleport back to the surface (floor 0).
 *
 * Landing points:
 *   - surface: first free walkable tile in a spiral around the city center;
 *   - dungeon: a STAIRS_UP tile (the same landing used by the real stairs),
 *     falling back to a spiral search for a free floor tile.
 * ==========================================================================*/

/*Spiral search around (cx,cy) for a free, walkable tile.
 * Returns true and stores the result in *ox,*oy.*/
static bool transit_find_free_tile(Floor *fl, int cx, int cy, int *ox, int *oy) {
    for (int r = 0; r < 80; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                /*Only walk the ring of this radius (r=0 is the center)*/
                if (r > 0 && abs(dx) != r && abs(dy) != r) {
                    continue;
                }
                int nx = cx + dx;
                int ny = cy + dy;
                if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
                    continue;
                }
                VoxelType vt = fl->map.data[0][ny][nx];
                if (vt == VOXEL_WALL || vt == VOXEL_ROCK) {
                    continue;
                }
                if (fl->entity_grid[ny][nx] != 0) {
                    continue;
                }
                *ox = nx;
                *oy = ny;
                return true;
            }
        }
    }
    return false;
}

/*Dungeon landing: prefer a free STAIRS_UP tile (consistent with descending
 * the real stairs); accept an occupied one as last resort.*/
static bool transit_find_dungeon_entry(Floor *fl, int *ox, int *oy) {
    int any_x = -1, any_y = -1;
    for (int iy = 0; iy < MAP_HEIGHT; iy++) {
        for (int ix = 0; ix < MAP_WIDTH; ix++) {
            if (fl->map.data[0][iy][ix] != VOXEL_STAIRS_UP) {
                continue;
            }
            if (any_x < 0) {
                any_x = ix;
                any_y = iy;
            }
            if (fl->entity_grid[iy][ix] == 0) {
                *ox = ix;
                *oy = iy;
                return true;
            }
        }
    }
    if (any_x >= 0) {
        *ox = any_x;
        *oy = any_y;
        return true;
    }
    return false;
}

/*-----------------------------------------------------------------------------
 * handle_class_transit — surface <-> deepest explored floor.
 * Shared by the 12 class transit cantrips (Ancestral Totem Call, The Last
 * Refrain, Divine Recall, The Green Path, Warrior's March, Flowing Step,
 * Oathbound Passage, Hunter's Trail, The Back Door, Bloodline Rift,
 * Pact Passage, Arcane Transit).
 * -------------------------------------------------------------------------*/
static bool handle_class_transit(SpellContext *ctx) {
    Game *game = ctx->game;
    Client *c = ctx->caster;
    if (!c || !game->world) {
        return true;
    }

    /*--- Decide the destination floor ---*/
    int target_floor;
    if (c->floor_id == 0) {
        if (c->max_floor_explored <= 0) {
            send_text_to_client(c->sock,
                                "[MAGIC] %s: the way is not yet known. Descend into the"
                                " dungeon and explore at least one level before you can"
                                " call it back.",
                                ctx->sp->name);
            return true;
        }
        if (c->max_floor_explored >= MAX_FLOORS) {
            c->max_floor_explored = MAX_FLOORS - 1;
        }
        target_floor = c->max_floor_explored;
    } else {
        target_floor = 0;
    }

    Floor *fl = &game->world->floors[target_floor];

    /*--- Find a safe landing tile ---*/
    int lx = -1, ly = -1;
    if (target_floor == 0) {
        if (!transit_find_free_tile(fl, MAP_CENTER_X, MAP_CENTER_Y, &lx, &ly)) {
            send_text_to_client(c->sock, "[MAGIC] %s: the path collapses before you can cross it!",
                                ctx->sp->name);
            return true;
        }
    } else {
        if (!transit_find_dungeon_entry(fl, &lx, &ly) &&
            !transit_find_free_tile(fl, MAP_CENTER_X, MAP_CENTER_Y, &lx, &ly)) {
            send_text_to_client(c->sock, "[MAGIC] %s: the path collapses before you can cross it!",
                                ctx->sp->name);
            return true;
        }
    }

    /*--- Perform the transit ---*/
    game->world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
    int old_floor = c->floor_id;
    int old_x = c->x, old_y = c->y;

    c->floor_id = target_floor;
    c->x = lx;
    c->y = ly;
    fl->entity_grid[ly][lx] = c->entity_id;
    client_track_explored_floor(c);

    /*The player changed floor: remove him from the clients still on the
    * old floor (hp<=0 message) and announce him to the new floor.*/
    notify_player_left_floor(game, c, old_floor);
    broadcast_player_state(game, c);

    if (target_floor > old_floor) {
        send_text_to_client(c->sock,
                            "[MAGIC] %s: the world folds around you... you emerge on Floor %d"
                            " (%d,%d).",
                            ctx->sp->name, target_floor, lx, ly);
    } else {
        send_text_to_client(c->sock,
                            "[MAGIC] %s: the world folds around you... you emerge back on"
                            " the surface (%d,%d).",
                            ctx->sp->name, lx, ly);
    }
    send_text_to_client(c->sock, "  > From Floor %d (%d,%d) to Floor %d (%d,%d).", old_floor, old_x,
                        old_y, c->floor_id, c->x, c->y);

    /*--- Sync the client with the new floor ---*/
    send_detailed_state(game, c);
    send_map_chunk(c->sock, &fl->map, c->x, c->y, INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, game);
    save_player_data(c);

    server_log("MAGIC", "%s used %s: floor %d -> %d (%d,%d).", c->username, ctx->sp->name,
               old_floor, target_floor, lx, ly);
    return true;
}
