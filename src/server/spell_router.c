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
 * Categories implemented:
 * 1. TERRAIN — Edit voxels on the map (Wall of Stone, Mud, Ice)
 * 2. SUMMON — Summons a new NPC ally next to the caster
 * 3. CHARM — Change the faction of a target NPC
 * 4. UTILITY — Teleport, Invisibility and other special effects*/

#include "spell_router.h"
#include "server_internal.h"
#include "server_spawn.h"
#include "pathfinding.h"
#include "map.h"
#include "rules.h"
#include "bestiary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdbool.h>

/* =========================================================================
 * Internal prototypes
 * ========================================================================= */
static bool handle_wall_of_stone(SpellContext *ctx);
static bool handle_mud_field(SpellContext *ctx);
static bool handle_ice_storm_terrain(SpellContext *ctx);
static bool handle_summon_elemental(SpellContext *ctx);
static bool handle_animate_dead(SpellContext *ctx);
static bool handle_charm_person(SpellContext *ctx);
static bool handle_dominate_monster(SpellContext *ctx);
static bool handle_teleport(SpellContext *ctx);
static bool handle_invisibility(SpellContext *ctx);
static bool handle_blink(SpellContext *ctx);
static bool handle_class_transit(SpellContext *ctx);

/*============================================================================
 * Lookup table hook: {exact_spell_name, handler}
* MUST be kept in alphabetical order (spell_router_init() sorts it)
 * ==========================================================================*/
static SpellHook g_spell_hooks[] = {
    { "Animate Dead",         handle_animate_dead       },
    { "Arcane Transit",       handle_class_transit      },
    { "Ancestral Totem Call", handle_class_transit      },
    { "Bloodline Rift",       handle_class_transit      },
    { "Blink",                handle_blink              },
    { "Charm Person",         handle_charm_person       },
    { "Divine Recall",        handle_class_transit      },
    { "Flowing Step",         handle_class_transit      },
    { "Dominate Monster",     handle_dominate_monster   },
    { "Ice Storm",            handle_ice_storm_terrain  },
    { "Hunter's Trail",       handle_class_transit      },
    { "Invisibility",         handle_invisibility       },
    { "Mud Field",            handle_mud_field          },
    { "Oathbound Passage",    handle_class_transit      },
    { "Pact Passage",         handle_class_transit      },
    { "Summon Elemental",     handle_summon_elemental   },
    { "Teleport",             handle_teleport           },
    { "The Back Door",        handle_class_transit      },
    { "The Green Path",       handle_class_transit      },
    { "The Last Refrain",     handle_class_transit      },
    { "Warrior's March",      handle_class_transit      },
    { "Wall of Stone",        handle_wall_of_stone      },
};

static int g_hook_count = (int)(sizeof(g_spell_hooks) / sizeof(g_spell_hooks[0]));
static bool g_router_ready = false;

/*============================================================================
 * Comparator for qsort/bsearch (case-insensitive)
 * ==========================================================================*/
static int hook_cmp(const void *a, const void *b) {
    const SpellHook *ha = (const SpellHook *)a;
    const SpellHook *hb = (const SpellHook *)b;
    return strcasecmp(ha->name, hb->name);
}

/*============================================================================
 * spell_router_init
 * Order g_spell_hooks only once to enable binary search.
 * ==========================================================================*/
void spell_router_init(void) {
    qsort(g_spell_hooks, (size_t)g_hook_count, sizeof(SpellHook), hook_cmp);
    g_router_ready = true;
    server_log("MAGIC", "Magic Router initialized: %d hooks registered.", g_hook_count);
}

/*============================================================================
 * spell_router_dispatch
* O(log N) binary search in table. Returns true if handled.
 * ==========================================================================*/
bool spell_router_dispatch(SpellContext *ctx) {
    if (!g_router_ready || !ctx || !ctx->sp) {
        return false;
    }

    SpellHook key;
    key.name    = ctx->sp->name;
    key.handler = NULL;

    SpellHook *found = (SpellHook *)bsearch(
        &key,
        g_spell_hooks,
        (size_t)g_hook_count,
        sizeof(SpellHook),
        hook_cmp
    );

    if (!found) {
        return false;
    }

    return found->handler(ctx);
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
static int spell_terrain_stamp(Floor *floor, int cx, int cy, int radius,
                                VoxelType new_type, bool only_floor) {
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

/*-----------------------------------------------------------------------
 * Wall of Stone — Causes walls of stone to emerge around the target.
 * Mechanic: Transform VOXEL_FLOORs adjacent to the target into VOXEL_WALLs.
 * The caster is never blocked (his cell remains free).
 * -------------------------------------------------------------------------*/
static bool handle_wall_of_stone(SpellContext *ctx) {
    Client *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;

    int tx = c->x;
    int ty = c->y;

    if (ctx->target) {
        tx = ctx->target->x;
        ty = ctx->target->y;
    }

    Floor *floor = &master_world->floors[c->floor_id];

    int radius = (sp->radius > 0) ? sp->radius : 2;
    int changed = spell_terrain_stamp(floor, tx, ty, radius, VOXEL_WALL, true);

    /*The caster cell always remains passable*/
    floor->map.data[0][c->y][c->x] = VOXEL_FLOOR;

    send_text_to_client(c->sock,
        "[MAGIC] Wall of Stone: A wall of living rock emerges from the ground!");
    send_text_to_client(c->sock,
        "> %d floor cells solidify into rough stone.",
        changed);

    server_log("MAGIC", "%s casts Wall of Stone at (%d,%d) [radius %d, %d cells].",
               c->username, tx, ty, radius, changed);

    return true;
}

/* -------------------------------------------------------------------------
 * Mud Field — Transforms the terrain into mud that slows enemies.
 * Mechanic: Converts VOXEL_FLOOR to VOXEL_MUD within a radius of the target.
 * Mud causes movement penalties (handled by the server_world tick).
 * ------------------------------------------------------------------------- */
static bool handle_mud_field(SpellContext *ctx) {
    Client *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;

    int tx = c->x;
    int ty = c->y;

    if (ctx->target) {
        tx = ctx->target->x;
        ty = ctx->target->y;
    }

    Floor *floor = &master_world->floors[c->floor_id];

    int radius = (sp->radius > 0) ? sp->radius : 3;
    int changed = spell_terrain_stamp(floor, tx, ty, radius, VOXEL_MUD, true);

    send_text_to_client(c->sock,
        "[MAGIC] Mud Field: The ground turns into sticky quagmires!");
    send_text_to_client(c->sock,
        "> %d cells are swallowed by the mud. Enemies will slow down.",
        changed);

    return true;
}

/* -------------------------------------------------------------------------
 * Ice Storm (Terrain variant) — Covers the terrain in ice.
 * Mechanics: Converts floor tiles to VOXEL_ICE. Ice can cause slipping
 * (Dexterity penalty, handled in movement ticks).
 * Note: the spell's base damage effect (AoE) is handled by the standard AoE
 * system; this hook adds ONLY the terrain modification.
 * ------------------------------------------------------------------------- */
static bool handle_ice_storm_terrain(SpellContext *ctx) {
    Client *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;

    int tx = c->x;
    int ty = c->y;

    if (ctx->target) {
        tx = ctx->target->x;
        ty = ctx->target->y;
    }

    Floor *floor = &master_world->floors[c->floor_id];

    int radius = (sp->radius > 0) ? sp->radius : 4;
    int changed = spell_terrain_stamp(floor, tx, ty, radius, VOXEL_ICE, true);

    send_text_to_client(c->sock,
        "[MAGIC] Ice Storm: the storm transforms the ground into a sheet of ice!");
    send_text_to_client(c->sock,
        "> %d cells are covered in slippery ice.", changed);

    /*
     * After modifying the terrain, we delegate the AoE damage to the
     * standard system by calling aoe_resolve_spell. This hook is NOT to be
     * considered a final endpoint: we return true to allow the processing
     * to continue.
     * NOTE: In this specific case we return true because we want
     * full control over the message. AoE damage is applied separately
     * by the calling logic if target_type is AOE.
     */
    return true;
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
 * find_free_npc_slot — Searches for a free slot in the global NPC array.
 * Returns the pointer to the first NPC with active == false, or NULL.*/
static NPC *find_free_npc_slot(NPC *npcs) {
    for (int i = 0; i < MAX_NPCS; i++) {
        if (!npcs[i].active) {
            return &npcs[i];
        }
    }
    return NULL;
}

/**
 * find_spawn_cell — Searches for a free VOXEL_FLOOR cell adjacent to the caster.
 *Try the 8 cardinal + diagonal directions, then expand the radius.
 * Returns true and sets *out_x, *out_y if found.*/
static bool find_spawn_cell(Client *c, int *out_x, int *out_y) {
    Floor *floor = &master_world->floors[c->floor_id];

    static const int dx[] = { 0,  1,  0, -1,  1, -1,  1, -1 };
    static const int dy[] = {-1,  0,  1,  0, -1,  1,  1, -1 };

    for (int i = 0; i < 8; i++) {
        int nx = c->x + dx[i];
        int ny = c->y + dy[i];

        if (nx < 1 || nx >= MAP_WIDTH - 1 || ny < 1 || ny >= MAP_HEIGHT - 1) {
            continue;
        }

        if (floor->map.data[0][ny][nx] == VOXEL_FLOOR &&
            floor->entity_grid[ny][nx] == 0) {
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

            if (floor->map.data[0][ny][nx] == VOXEL_FLOOR &&
                floor->entity_grid[ny][nx] == 0) {
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
    Client *c   = ctx->caster;
    NPC   *npcs = ctx->npcs;

    NPC *slot = find_free_npc_slot(npcs);
    if (!slot) {
        send_text_to_client(c->sock,
            "[MAGIC] The ethereal plane is overcrowded: summoning failed!");
        return true;
    }

    int sx = 0;
    int sy = 0;

    if (!find_spawn_cell(c, &sx, &sy)) {
        send_text_to_client(c->sock,
            "[MAGIC] No free space to summon the elemental!");
        return true;
    }

    /*Choose the elemental type based on the voxel under the caster*/
    Floor *floor = &master_world->floors[c->floor_id];
    VoxelType under = floor->map.data[0][c->y][c->x];

    const char *elem_name = "Earth Elemental";
    int elem_hp   = 40;
    int elem_atk  = 5;
    int elem_dmg  = 6;
    int elem_ac   = 13;

    if (under == VOXEL_LAVA || under == VOXEL_ASH) {
        elem_name = "Fire Elemental";
        elem_hp   = 52;
        elem_atk  = 7;
        elem_dmg  = 8;
        elem_ac   = 12;
    } else if (under == VOXEL_WATER || under == VOXEL_ICE) {
        elem_name = "Water Elemental";
        elem_hp   = 46;
        elem_atk  = 6;
        elem_dmg  = 6;
        elem_ac   = 14;
    } else if (under == VOXEL_GRASS || under == VOXEL_FLOOR) {
        elem_name = "Air Elemental";
        elem_hp   = 38;
        elem_atk  = 8;
        elem_dmg  = 6;
        elem_ac   = 15;
    }

    /* Initialize the summoned NPC */
    memset(slot, 0, sizeof(NPC));
    slot->entity_id       = next_id++;
    slot->active          = true;
    slot->archetype       = ARCH_MELEE;
    slot->template        = NULL;
    slot->template_idx    = -1;
    slot->x               = sx;
    slot->y               = sy;
    slot->floor_id        = c->floor_id;
    slot->hp              = elem_hp;
    slot->max_hp          = elem_hp;
    slot->ac              = elem_ac;
    slot->attack_bonus    = elem_atk;
    slot->damage_dice     = elem_dmg;
    slot->damage_sides    = 1; /* fixed damage: flat elem_dmg */
    slot->xp_reward       = 0; /*Summoning yields no XP*/
    slot->gold_drop       = 0;
    slot->morale          = 100;
    slot->energy          = 0;
    slot->spawn_x         = sx;
    slot->spawn_y         = sy;
    slot->respawn_timer   = 0;

    /*Ally Marker: The target_id points to the caster's entity_id*/
    slot->ai_ctx.current_target_id = c->entity_id;

    snprintf(slot->custom_name, sizeof(slot->custom_name),
             "[SUMMONED] %s", elem_name);

    /* Update the entity grid */
    floor->entity_grid[sy][sx] = slot->entity_id;

    send_text_to_client(c->sock,
        "[MAGIC] Summon Elemental: a %s materializes from the ethereal plane!", elem_name);
    send_text_to_client(c->sock,
        "  > The entity (HP:%d AC:%d) will follow you into battle.", elem_hp, elem_ac);

    server_log("MAGIC", "%s summons %s (ID:%d) at (%d,%d) floor %d.",
               c->username, elem_name, slot->entity_id, sx, sy, c->floor_id);

    return true;
}

/*-----------------------------------------------------------------------
 * Animate Dead — Summons a skeleton from the remains of a dead NPC.
 * Finds the nearest inactive NPC on the same floor.
 * -------------------------------------------------------------------------*/
static bool handle_animate_dead(SpellContext *ctx) {
    Client *c   = ctx->caster;
    NPC   *npcs = ctx->npcs;

    /*Search for a nearby corpse (NPC not active with valid template)*/
    NPC *corpse = NULL;
    int  min_d  = 8; /*maximum summon range*/

    for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active) {
            continue;
        }
        if (npcs[i].template == NULL) {
            continue;
        }
        if (npcs[i].floor_id != c->floor_id) {
            continue;
        }

        int d = abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y);
        if (d < min_d) {
            min_d  = d;
            corpse = &npcs[i];
        }
    }

    if (!corpse) {
        send_text_to_client(c->sock,
            "[MAGIC] Animate Dead: There are no corpses nearby to reanimate.");
        return true;
    }

    /*Reactivate the corpse as an undead ally*/
    corpse->active              = true;
    corpse->hp                  = corpse->max_hp / 2; /* revives with half HP */
    corpse->xp_reward           = 0;
    corpse->gold_drop           = 0;
    corpse->ai_ctx.current_target_id = c->entity_id; /* allied faction */

    const char *original = corpse->template ? corpse->template->name : "Creature";
    snprintf(corpse->custom_name, sizeof(corpse->custom_name),
             "[SUMMONED] Skeleton of %s", original);

    Floor *floor = &master_world->floors[c->floor_id];
    floor->entity_grid[corpse->y][corpse->x] = corpse->entity_id;

    send_text_to_client(c->sock,
        "[MAGIC] Animate Dead: The bones of the %s rise at your command!", original);
    send_text_to_client(c->sock,
        "  > The skeleton (HP:%d) is ready to fight for you.",
        corpse->hp);

    return true;
}

/* =========================================================================
 * ─────────────────────────────────────────────────────────────────────────
 *  CATEGORY 3: CHARM / MENTAL CONTROL
 * ─────────────────────────────────────────────────────────────────────────
 * ========================================================================= */

/*-----------------------------------------------------------------------
 * Charm Person — Charms a target humanoid temporarily
 * ally. The target stops attacking the caster for N rounds.
 * Mechanic: Set current_target_id = -1 (no target) and apply
 * the "Charmed" status effect that suppresses aggression.
 * -------------------------------------------------------------------------*/
static bool handle_charm_person(SpellContext *ctx) {
    Client       *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;
    NPC          *tgt = ctx->target;

    if (!tgt) {
        send_text_to_client(c->sock,
            "[MAGIC] Charm Person: No targets in range.");
        return true;
    }

    /*Wisdom saving throw against the caster's DC*/
    int dc = 8 + rules_get_modifier(c->intel) + 2; /* +2 proficiency */
    bool saved = rules_roll_save(10, dc, false, false, NULL);

    if (saved) {
        send_text_to_client(c->sock,
            "[MAGIC] %s resists your charm! (Successful save)",
            tgt->template ? tgt->template->name : "???");
        return true;
    }

    /*Apply the charm: reset the AI ​​target and mark as charmed*/
    tgt->ai_ctx.current_target_id = -1; /*no active targets*/

    if (tgt->effect_count < MAX_EFFECTS_PER_ENTITY) {
        ActiveEffect charm_fx;
        memset(&charm_fx, 0, sizeof(charm_fx));
        charm_fx.name = "Charmed";
        charm_fx.duration_rounds = sp->status_effect.duration_rounds > 0
                                   ? sp->status_effect.duration_rounds
                                   : 5;
        tgt->effects[tgt->effect_count++] = charm_fx;
    }

    send_text_to_client(c->sock,
        "[MAGIC] Charm Person: %s gazes at you dreamily... they are under your control!",
        tgt->template ? tgt->template->name : "???");
    send_text_to_client(c->sock,
        "> Effect lasts %d rounds (Failed Sag DC%d save).",
        sp->status_effect.duration_rounds > 0 ? sp->status_effect.duration_rounds : 5,
        dc);

    server_log("MAGIC", "%s charms %s (ID:%d) with Charm Person.",
               c->username,
               tgt->template ? tgt->template->name : "???",
               tgt->entity_id);

    return true;
}

/*-----------------------------------------------------------------------
 * Dominate Monster — Same as Charm Person but without saving throw and on any
* creatures. The creature becomes a permanent ally until the end of
 * combat (or until it takes damage from the caster).
 * -------------------------------------------------------------------------*/
static bool handle_dominate_monster(SpellContext *ctx) {
    Client       *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;
    NPC          *tgt = ctx->target;

    if (!tgt) {
        send_text_to_client(c->sock,
            "[MAGIC] Dominate Monster: No targets in range.");
        return true;
    }

    /*Wisdom saving throw with disadvantage (roll 2 dice, take the worst)*/
    int dc = 8 + rules_get_modifier(c->intel) + 2;
    int roll1 = rules_roll_dice(1, 20);
    int roll2 = rules_roll_dice(1, 20);
    int worst = (roll1 < roll2) ? roll1 : roll2;
    int mod   = rules_get_modifier(10); /* Base NPC Wisdom (10 = no modifier) */
    bool saved = (worst + mod >= dc);

    if (saved) {
        send_text_to_client(c->sock,
            "[MAGIC] %s resists domination! (Save with disadvantage: %d vs DC%d)",
            tgt->template ? tgt->template->name : "???",
            worst + mod,
            dc);
        return true;
    }

    tgt->ai_ctx.current_target_id = c->entity_id; /* full allied faction */

    if (tgt->effect_count < MAX_EFFECTS_PER_ENTITY) {
        ActiveEffect dom_fx;
        memset(&dom_fx, 0, sizeof(dom_fx));
        dom_fx.name = "Dominated";
        dom_fx.duration_rounds = sp->status_effect.duration_rounds > 0
                                 ? sp->status_effect.duration_rounds
                                 : 10;
        tgt->effects[tgt->effect_count++] = dom_fx;
    }

    snprintf(tgt->custom_name, sizeof(tgt->custom_name),
             "[DOMINATED] %s",
             tgt->template ? tgt->template->name : "Creature");

    send_text_to_client(c->sock,
        "[MAGIC] Dominate Monster: %s's mind yields to your will!",
        tgt->template ? tgt->template->name : "???");
    send_text_to_client(c->sock,
        "  > The creature will fight for you for %d rounds.",
        sp->status_effect.duration_rounds > 0 ? sp->status_effect.duration_rounds : 10);

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
    Client *c = ctx->caster;
    Floor  *floor = &master_world->floors[c->floor_id];

    /* Collect available free cells */
    int free_xs[256];
    int free_ys[256];
    int free_count = 0;

    /*Sparse sampling for performance (checks 1/16 of cells)*/
    for (int y = 2; y < MAP_HEIGHT - 2 && free_count < 256; y += 4) {
        for (int x = 2; x < MAP_WIDTH - 2 && free_count < 256; x += 4) {
            if (floor->map.data[0][y][x] == VOXEL_FLOOR &&
                floor->entity_grid[y][x] == 0) {
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

    send_text_to_client(c->sock,
        "[MAGIC] Teleport: a rip in the ether sucks you in...");
    send_text_to_client(c->sock,
        "> You materialize in (%d,%d). [Previous: (%d,%d)]",
        c->x, c->y, old_x, old_y);

    server_log("MAGIC", "%s teleports from (%d,%d) to (%d,%d) [floor %d].",
               c->username, old_x, old_y, c->x, c->y, c->floor_id);

    return true;
}

/*-----------------------------------------------------------------------
 * Invisibility — Makes the caster invisible for N rounds.
 * Mechanic: Adds "Invisible" status to client effects.
 * The AI ​​in ai.c ignores targets with this status in target calculation.
 * -------------------------------------------------------------------------*/
static bool handle_invisibility(SpellContext *ctx) {
    Client       *c  = ctx->caster;
    SpellTemplate *sp = ctx->sp;

    if (c->effect_count >= MAX_EFFECTS_PER_ENTITY) {
        send_text_to_client(c->sock,
            "[MAGIC] You cannot sustain other magical effects at the same time.");
        return true;
    }

    /* Check if already invisible */
    for (int i = 0; i < c->effect_count; i++) {
        if (strncmp(c->effects[i].name, "Invisible", 9) == 0) {
            send_text_to_client(c->sock,
                "[MAGIC] You are already invisible (%d rounds remaining).",
                c->effects[i].duration_rounds);
            return true;
        }
    }

    ActiveEffect invis_fx;
    memset(&invis_fx, 0, sizeof(invis_fx));
    invis_fx.name = "Invisible";
    invis_fx.duration_rounds = sp->status_effect.duration_rounds > 0
                               ? sp->status_effect.duration_rounds
                               : 5;
    c->effects[c->effect_count++] = invis_fx;

    send_text_to_client(c->sock,
        "[MAGIC] Invisibility: Your body dissolves into the air!");
    send_text_to_client(c->sock,
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
    Client *c     = ctx->caster;
    Floor  *floor = &master_world->floors[c->floor_id];

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

        if (floor->map.data[0][ny][nx] == VOXEL_FLOOR &&
            floor->entity_grid[ny][nx] == 0) {
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

    send_text_to_client(c->sock,
        "[MAGIC] Blink: a flash of light and you vanish... only to reappear %d cells away!",
        blink_dist);
    send_text_to_client(c->sock,
        "> From (%d,%d) to (%d,%d).", old_x, old_y, c->x, c->y);

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
static bool transit_find_free_tile(Floor *fl, int cx, int cy,
                                   int *ox, int *oy) {
    for (int r = 0; r < 80; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                /*Only walk the ring of this radius (r=0 is the center)*/
                if (r > 0 && abs(dx) != r && abs(dy) != r)
                    continue;
                int nx = cx + dx;
                int ny = cy + dy;
                if (nx < 1 || nx >= MAP_WIDTH - 1 ||
                    ny < 1 || ny >= MAP_HEIGHT - 1)
                    continue;
                VoxelType vt = fl->map.data[0][ny][nx];
                if (vt == VOXEL_WALL || vt == VOXEL_ROCK)
                    continue;
                if (fl->entity_grid[ny][nx] != 0)
                    continue;
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
            if (fl->map.data[0][iy][ix] != VOXEL_STAIRS_UP)
                continue;
            if (any_x < 0) { any_x = ix; any_y = iy; }
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
 * Shared by the 12 innate class cantrips (Ancestral Totem Call, The Last
 * Refrain, Divine Recall, The Green Path, Warrior's March, Flowing Step,
 * Oathbound Passage, Hunter's Trail, The Back Door, Bloodline Rift,
 * Pact Passage, Arcane Transit).
 * -------------------------------------------------------------------------*/
static bool handle_class_transit(SpellContext *ctx) {
    Client *c = ctx->caster;
    if (!c || !master_world)
        return true;

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
        if (c->max_floor_explored >= MAX_FLOORS)
            c->max_floor_explored = MAX_FLOORS - 1;
        target_floor = c->max_floor_explored;
    } else {
        target_floor = 0;
    }

    Floor *fl = &master_world->floors[target_floor];

    /*--- Find a safe landing tile ---*/
    int lx = -1, ly = -1;
    if (target_floor == 0) {
        if (!transit_find_free_tile(fl, MAP_CENTER_X, MAP_CENTER_Y, &lx, &ly)) {
            send_text_to_client(c->sock,
                "[MAGIC] %s: the path collapses before you can cross it!",
                ctx->sp->name);
            return true;
        }
    } else {
        if (!transit_find_dungeon_entry(fl, &lx, &ly) &&
            !transit_find_free_tile(fl, MAP_CENTER_X, MAP_CENTER_Y, &lx, &ly)) {
            send_text_to_client(c->sock,
                "[MAGIC] %s: the path collapses before you can cross it!",
                ctx->sp->name);
            return true;
        }
    }

    /*--- Perform the transit ---*/
    master_world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
    int old_floor = c->floor_id;
    int old_x = c->x, old_y = c->y;

    c->floor_id = target_floor;
    c->x = lx;
    c->y = ly;
    fl->entity_grid[ly][lx] = c->entity_id;
    client_track_explored_floor(c);

    /*The player changed floor: remove him from the clients still on the
    * old floor (hp<=0 message) and announce him to the new floor.*/
    notify_player_left_floor(c, old_floor);
    broadcast_player_state(c);

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
    send_text_to_client(c->sock,
        "  > From Floor %d (%d,%d) to Floor %d (%d,%d).",
        old_floor, old_x, old_y, c->floor_id, c->x, c->y);

    /*--- Sync the client with the new floor ---*/
    send_detailed_state(c);
    send_map_chunk(c->sock, &fl->map, c->x, c->y, INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, g_npcs);
    save_player_data(c);

    server_log("MAGIC", "%s used %s: floor %d -> %d (%d,%d).",
               c->username, ctx->sp->name, old_floor, target_floor, lx, ly);
    return true;
}
