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
 * aoe.c — Area of Effect engine for DragonGL
 *
 * Implemented projection types:
 *  - AOE_CIRCLE  : Spherical explosion (Fireball, Ice Storm)
 *  - AOE_CONE    : Cone (Dragon Breath, Burning Spray, Cone of Cold)
 *  - AOE_LINE    : Line (Lightning Bolt, Acid Spit, Gas Jet)
 *  - AOE_CLOUD   : Persistent multi-round cloud (Poison Gas, Acid Cloud)
 *
 * Each type has unique narrative text for textual immersion.
 */

#include "aoe.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../include/map.h"
#include "server_internal.h"

extern World *master_world;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "aoe.h"
#include "combat_log.h"

//Global pool of persistent clouds
PersistentCloud g_clouds[MAX_CLOUDS];

// -------------------------------------------------------
// NARRATIVE text for damage type
// -------------------------------------------------------
static const char* dmg_type_name(DamageType t) {
    switch (t) {
        case DMG_FIRE:        return "fire";
        case DMG_ACID:        return "acid";
        case DMG_POISON:      return "poison";
        case DMG_COLD:        return "ice";
        case DMG_LIGHTNING:   return "lightning";
        case DMG_THUNDER:     return "thunder";
        case DMG_NECROTIC:    return "necrotic energy";
        case DMG_RADIANT:     return "radiant light";
        case DMG_FORCE:       return "arcane force";
        case DMG_PSYCHIC:     return "psychic power";
        case DMG_SLASHING:    return "slashing";
        case DMG_PIERCING:    return "piercing";
        case DMG_BLUDGEONING: return "bludgeoning";
        default:              return "dark energy";
    }
}

// -------------------------------------------------------
// INITIALIZATION
// -------------------------------------------------------
void aoe_init_clouds(void) {
    memset(g_clouds, 0, sizeof(g_clouds));
}

// -------------------------------------------------------
// HELPER: apply damage to a single NPC with narrative text
// -------------------------------------------------------
void aoe_apply_damage_to_npc(
    NPC        *npc,
    Client     *caster,
    int         dmg,
    bool        saved,
    const char *effect_name,
    const char *kill_msg
) {
    if (!npc || !npc->active || !caster) {
        return;
    }

    npc->hp -= dmg;

    send_text_to_client(caster->sock,
        "  > %s: %d %s damage%s (HP: %d/%d)",
        npc->template ? npc->template->name : "???",
        dmg,
        effect_name,
        saved ? " [Save! -half]" : "",
        npc->hp,
        npc->max_hp);

    if (npc->hp <= 0) {
        npc->active       = false;
        npc->respawn_timer = 120; //RESPAWN_TICKS
        int xp_gained    = npc->template ? npc->template->xp : 10;
        caster->xp       += xp_gained;
        send_text_to_client(caster->sock, kill_msg,
            npc->template ? npc->template->name : "???",
            xp_gained);
        clog_death(
            npc->template ? npc->template->name : "???",
            caster->username,
            npc->floor_id);
        if (npc->archetype == ARCH_BOSS) {
            handle_boss_death(caster, npc);
        }
    }
}

// -------------------------------------------------------
//HELPER: create persistent cloud
// -------------------------------------------------------
static void spawn_cloud(SpellTemplate *sp, int cx, int cy, int floor_id, int save_dc) {
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!g_clouds[i].active) {
            g_clouds[i].active      = true;
            g_clouds[i].cx          = cx;
            g_clouds[i].cy          = cy;
            g_clouds[i].radius      = sp->radius > 0 ? sp->radius : 3;
            g_clouds[i].floor_id    = floor_id;
            g_clouds[i].rounds_left = sp->cloud_rounds > 0 ? sp->cloud_rounds : 3;
            g_clouds[i].dice_count  = sp->dice_count > 0 ? sp->dice_count : 1;
            g_clouds[i].dice_sides  = sp->dice_sides > 0 ? sp->dice_sides : 6;
            g_clouds[i].dmg_type    = sp->damage_type;
            g_clouds[i].save_dc     = save_dc;
            strncpy(g_clouds[i].name, sp->name, 31);
            break;
        }
    }
}

// -------------------------------------------------------
//HELPER: Manhattan distance
// -------------------------------------------------------
static int mdist(int x1, int y1, int x2, int y2) {
    return abs(x1 - x2) + abs(y1 - y2);
}

// -------------------------------------------------------
//HELPER: squared Euclidean distance
// -------------------------------------------------------
static float edist_sq(int x1, int y1, int x2, int y2) {
    float dx = (float)(x1 - x2);
    float dy = (float)(y1 - y2);
    return dx * dx + dy * dy;
}

// -------------------------------------------------------
//HELPER: point falls within cone (90° angle, origin -> target)
// -------------------------------------------------------
static bool in_cone(int ox, int oy, int tx, int ty, int px, int py, int length) {
    // Direction vector of the cone (from origin to first target)
    float dx = (float)(tx - ox);
    float dy = (float)(ty - oy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return false;
    }
    dx /= len;
    dy /= len;

    // Vector toward the point being tested
    float px2 = (float)(px - ox);
    float py2 = (float)(py - oy);
    float pdist = sqrtf(px2 * px2 + py2 * py2);
    if (pdist > (float)length) {
        return false;
    }
    if (pdist < 0.001f) {
        return true; // Same as origin
    }

    float px_n = px2 / pdist;
    float py_n = py2 / pdist;

    // Cosine of angle = dot product
    float cosangle = dx * px_n + dy * py_n;
    // Cone 90° -> half-angle 45° -> cos(45°) ≈ 0.707
    return cosangle >= 0.707f;
}

// -------------------------------------------------------
//HELPER: point falls online (width 1 cell)
// -------------------------------------------------------
static bool on_line(int ox, int oy, int tx, int ty, int px, int py, int length) {
    float dx = (float)(tx - ox);
    float dy = (float)(ty - oy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return false;
    }

    // Project the point onto the line
    float ex = (float)(px - ox);
    float ey = (float)(py - oy);

    float proj = (ex * dx + ey * dy) / len;
    if (proj < 0.0f || proj > (float)length) {
        return false;
    }

    // Perpendicular distance
    float cross = fabsf(ex * (dy / len) - ey * (dx / len));
    return cross <= 1.5f; // Width ~1 cell
}

// -------------------------------------------------------
// MAIN RESOLUTION
// -------------------------------------------------------
int aoe_resolve_spell(
    SpellTemplate *sp,
    Client        *caster,
    NPC           *npcs,
    int            npc_count,
    int            origin_x,
    int            origin_y
) {
    if (!sp || !caster || !npcs) {
        return 0;
    }

    int spell_save_dc = 8 + 2 + rules_get_modifier(caster->intel);
    int radius        = sp->radius > 0 ? sp->radius : 4;
    int length        = sp->radius > 0 ? sp->radius : 8;
    int dc_count      = sp->dice_count > 0 ? sp->dice_count : 2;
    int dc_sides      = sp->dice_sides > 0 ? sp->dice_sides : 6;
    int hits          = 0;

    // Find the nearest NPC to orient cone/line
    NPC *nearest = NULL;
    int  min_d   = 9999;
    for (int i = 0; i < npc_count; i++) {
        if (!npcs[i].active || npcs[i].archetype == ARCH_MERCHANT) {
            continue;
        }
        if (npcs[i].floor_id != caster->floor_id) {
            continue;
        }
        int d = mdist(origin_x, origin_y, npcs[i].x, npcs[i].y);
        if (d < min_d) {
            min_d   = d;
            nearest = &npcs[i];
        }
    }

    int target_x = nearest ? nearest->x : origin_x + 1;
    int target_y = nearest ? nearest->y : origin_y;

    // Launch narrative by projection type
    switch (sp->target_type) {

    case SPELL_TARGET_AOE_CIRCLE:
        send_text_to_client(caster->sock,
            "[ACTION] Spherical vortices of %s explode in a radius of %d cells around the impact point!",
            dmg_type_name(sp->damage_type), radius);
        break;

    case SPELL_TARGET_AOE_CONE:
        if (sp->damage_type == DMG_FIRE) {
            send_text_to_client(caster->sock,
                "[ACTION] You open your maw and a wave of DRACONIC FIRE incinerates everything in a %d-cell cone!",
                length);
        } else if (sp->damage_type == DMG_COLD) {
            send_text_to_client(caster->sock,
                "[ACTION] A frigid blast of arcane frost sweeps the cone ahead of you for %d cells!",
                length);
        } else if (sp->damage_type == DMG_ACID) {
            send_text_to_client(caster->sock,
                "[ACTION] You spit a trail of corrosive acid that drains everything in a %d-cell cone!",
                length);
        } else {
            send_text_to_client(caster->sock,
                "[ACTION] A cone of %s surges forward over %d cells!",
                dmg_type_name(sp->damage_type), length);
        }
        break;

    case SPELL_TARGET_AOE_LINE:
        if (sp->damage_type == DMG_LIGHTNING) {
            send_text_to_client(caster->sock,
                "[ACTION] A bolt of pure LIGHTNING streaks across the room in a straight line for %d cells!",
                length);
        } else if (sp->damage_type == DMG_ACID) {
            send_text_to_client(caster->sock,
                "[ACTION] You launch a corrosive acid spit that eats through everything in its path (%d cells)!",
                length);
        } else {
            send_text_to_client(caster->sock,
                "[ACTION] A jet of %s cuts in a straight line for %d cells!",
                dmg_type_name(sp->damage_type), length);
        }
        break;

    case SPELL_TARGET_AOE_CLOUD:
        if (sp->damage_type == DMG_POISON) {
            send_text_to_client(caster->sock,
                "[ACTION] A dense cloud of POISON GAS forms and lingers for %d rounds!",
                sp->cloud_rounds > 0 ? sp->cloud_rounds : 3);
        } else if (sp->damage_type == DMG_ACID) {
            send_text_to_client(caster->sock,
                "[ACTION] A pungent ACID CLOUD corrodes the air and persists for %d rounds!",
                sp->cloud_rounds > 0 ? sp->cloud_rounds : 3);
        } else {
            send_text_to_client(caster->sock,
                "[ACTION] A cloud of %s expands and will linger for %d rounds!",
                dmg_type_name(sp->damage_type),
                sp->cloud_rounds > 0 ? sp->cloud_rounds : 3);
        }
        //Create persistent cloud, then apply first-round damage
        spawn_cloud(sp, origin_x, origin_y, caster->floor_id, spell_save_dc);
        break;

    default:
        break;
    }

    // --- TERRAFORMING (Voxel Chemistry) ---
    if (master_world && caster->floor_id >= 0 && caster->floor_id < MAX_FLOORS) {
        Floor *fl = &master_world->floors[caster->floor_id];
        int min_x = origin_x - length; if (min_x < 1) min_x = 1;
        int max_x = origin_x + length; if (max_x >= MAP_WIDTH-1) max_x = MAP_WIDTH-2;
        int min_y = origin_y - length; if (min_y < 1) min_y = 1;
        int max_y = origin_y + length; if (max_y >= MAP_HEIGHT-1) max_y = MAP_HEIGHT-2;
        
        for (int y = min_y; y <= max_y; y++) {
            for (int x = min_x; x <= max_x; x++) {
                bool in_zone = false;
                switch (sp->target_type) {
                    case SPELL_TARGET_AOE_CIRCLE: in_zone = edist_sq((float)origin_x, (float)origin_y, (float)x, (float)y) <= (float)(radius * radius); break;
                    case SPELL_TARGET_AOE_CONE:   in_zone = in_cone(origin_x, origin_y, target_x, target_y, x, y, length); break;
                    case SPELL_TARGET_AOE_LINE:   in_zone = on_line(origin_x, origin_y, target_x, target_y, x, y, length); break;
                    case SPELL_TARGET_AOE_CLOUD:  in_zone = edist_sq((float)origin_x, (float)origin_y, (float)x, (float)y) <= (float)(radius * radius); break;
                    default: break;
                }
                
                if (in_zone) {
                    VoxelType t = fl->map.data[0][y][x];
                    if (sp->damage_type == DMG_FIRE) {
                        if (t == VOXEL_ICE) fl->map.data[0][y][x] = VOXEL_WATER;
                        else if (t == VOXEL_WOOD) fl->map.data[0][y][x] = VOXEL_ASH;
                        else if (t == VOXEL_WATER) fl->map.data[0][y][x] = VOXEL_MUD;
                    } else if (sp->damage_type == DMG_COLD) {
                        if (t == VOXEL_WATER) fl->map.data[0][y][x] = VOXEL_ICE;
                        else if (t == VOXEL_LAVA) fl->map.data[0][y][x] = VOXEL_OBSIDIAN;
                        else if (t == VOXEL_MUD) fl->map.data[0][y][x] = VOXEL_FLOOR;
                    } else if (sp->damage_type == DMG_ACID) {
                        if (t == VOXEL_WOOD || t == VOXEL_DOOR) fl->map.data[0][y][x] = VOXEL_FLOOR;
                    } else if (sp->damage_type == DMG_FORCE || sp->damage_type == DMG_BLUDGEONING) {
                        if (t == VOXEL_WALL || t == VOXEL_COBBLE) fl->map.data[0][y][x] = VOXEL_ROCK;
                        else if (t == VOXEL_CRYSTAL_BLUE || t == VOXEL_CRYSTAL_PURPLE) fl->map.data[0][y][x] = VOXEL_FLOOR;
                    }
                }
            }
        }
    }

    //--- Iterate through all NPCs and apply damage if in the area ---
    for (int i = 0; i < npc_count; i++) {
        NPC *n = &npcs[i];
        if (!n->active || n->archetype == ARCH_MERCHANT) {
            continue;
        }
        if (n->floor_id != caster->floor_id) {
            continue;
        }

        bool in_zone = false;

        switch (sp->target_type) {

        case SPELL_TARGET_AOE_CIRCLE:
            in_zone = edist_sq(origin_x, origin_y, n->x, n->y) <= (float)(radius * radius);
            break;

        case SPELL_TARGET_AOE_CONE:
            in_zone = in_cone(origin_x, origin_y, target_x, target_y, n->x, n->y, length);
            break;

        case SPELL_TARGET_AOE_LINE:
            in_zone = on_line(origin_x, origin_y, target_x, target_y, n->x, n->y, length);
            break;

        case SPELL_TARGET_AOE_CLOUD:
            in_zone = edist_sq(origin_x, origin_y, n->x, n->y) <= (float)(radius * radius);
            break;

        default:
            break;
        }

        if (!in_zone) {
            continue;
        }

        //Saving Throw (CON/DEX depending on type)
        bool saved = rules_roll_save(0, spell_save_dc, false, false, NULL);
        int  raw   = rules_roll_dice(dc_count, dc_sides);
        int  dmg   = saved ? raw / 2 : raw;
        if (dmg < 1) {
            dmg = 1;
        }

        aoe_apply_damage_to_npc(n, caster, dmg, saved,
            dmg_type_name(sp->damage_type),
            "[VICTORY] %s is reduced to ashes by %s! (+%d XP)");

        clog_spell(caster->username, sp->name,
            n->template ? n->template->name : "???",
            dmg, saved);
        hits++;
    }

    if (hits == 0) {
        send_text_to_client(caster->sock,
            "> The effect dissipates into the void... no target hit.");
    } else {
        send_text_to_client(caster->sock,
            "  > %d bersagli colpiti!", hits);
    }

    return hits;
}

// -------------------------------------------------------
//PERSISTENT CLOUDS UPDATE (called every round)
// -------------------------------------------------------
void aoe_update_clouds(NPC *npcs, int npc_count, Client *clients, int client_count) {
    for (int ci = 0; ci < MAX_CLOUDS; ci++) {
        PersistentCloud *cloud = &g_clouds[ci];
        if (!cloud->active) {
            continue;
        }

        cloud->rounds_left--;

        //Notify all clients on that plan
        for (int p = 0; p < client_count; p++) {
            if (!clients[p].active || !clients[p].authenticated) {
                continue;
            }
            if (clients[p].floor_id != cloud->floor_id) {
                continue;
            }
            if (cloud->rounds_left <= 0) {
                send_text_to_client(clients[p].sock,
                    "[ENVIRONMENT] The %s dissolves in the air.", cloud->name);
            } else {
                send_text_to_client(clients[p].sock,
                    "[ENVIRONMENT] The %s continues to burn! (%d rounds remaining)",
                    cloud->name, cloud->rounds_left);
            }
        }

        if (cloud->rounds_left <= 0) {
            cloud->active = false;
            continue;
        }

        //Apply damage to all NPCs in the cloud
        for (int ni = 0; ni < npc_count; ni++) {
            NPC *n = &npcs[ni];
            if (!n->active || n->archetype == ARCH_MERCHANT) {
                continue;
            }
            if (n->floor_id != cloud->floor_id) {
                continue;
            }
            float dsq = edist_sq(cloud->cx, cloud->cy, n->x, n->y);
            if (dsq > (float)(cloud->radius * cloud->radius)) {
                continue;
            }

            bool saved = rules_roll_save(0, cloud->save_dc, false, false, NULL);
            int  raw   = rules_roll_dice(cloud->dice_count, cloud->dice_sides);
            int  dmg   = saved ? raw / 2 : raw;
            if (dmg < 1) {
                dmg = 1;
            }

            n->hp -= dmg;

            // Notify the nearest client (cloud owner not available)
            for (int p = 0; p < client_count; p++) {
                if (!clients[p].active || !clients[p].authenticated) {
                    continue;
                }
                if (clients[p].floor_id != cloud->floor_id) {
                    continue;
                }
                send_text_to_client(clients[p].sock,
                    "[CLOUD] %s takes %d damage from %s%s (HP: %d/%d)",
                    n->template ? n->template->name : "???",
                    dmg,
                    cloud->name,
                    saved ? "[Salvation!]" : "",
                    n->hp,
                    n->max_hp);

                if (n->hp <= 0) {
                    n->active        = false;
                    n->respawn_timer = 120;
                    send_text_to_client(clients[p].sock,
                        "[CLOUDS] %s succumbs to lethal fumes!",
                        n->template ? n->template->name : "???");
                    clog_death(
                        n->template ? n->template->name : "???",
                        cloud->name,
                        n->floor_id);
                    if (n->archetype == ARCH_BOSS) {
                        handle_boss_death(NULL, n);
                    }
                }
                break; //Notify only the first client on the plan
            }
        }
    }
}
