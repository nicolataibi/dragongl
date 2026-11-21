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
 * server_traps.c - Traps: detection, the trap that fires, its saving throw, damage and
 * effect on the world.
 */

#include "server_traps.h"
#include "item_effects.h"
#include "server_internal.h"

#include "player.h"
#include "server_tiles.h"

/*Passive perception (10 + WIS modifier) against the detection DC of a
 * trap within 2 tiles: a found floor trap becomes visible on the map.*/
static void trap_detect(Client *c, Trap *t, Floor *f) {
    int passive_perc = 10 + rules_get_modifier(c->wis);

    // DART_WALL detection: player senses the tiny hole in the wall
    if (t->type == TRAP_DART_WALL && !t->detected) {
        //Dart fires FROM wall tile (t->x, t->y) into adjacent floor tile
        int wall_dx[4] = {0, 1, 0, -1};
        int wall_dy[4] = {-1, 0, 1, 0};
        int fire_x = t->x + wall_dx[t->wall_dir];
        int fire_y = t->y + wall_dy[t->wall_dir];
        // Detect if player is within 2 tiles of the floor-side trigger tile
        if (abs(c->x - fire_x) <= 2 && abs(c->y - fire_y) <= 2) {
            if (passive_perc >= t->detection_dc) {
                t->detected = true;
                send_text_to_client(c->sock, "[SYSTEM] You notice a tiny hole in the wall..."
                                             "It looks like the launch tube of a dart trap!");
            }
        }
    } else if (!t->detected && (abs(c->x - t->x) <= 2 && abs(c->y - t->y) <= 2)) {
        if (passive_perc >= t->detection_dc) {
            t->detected = true;
            f->map.data[0][t->y][t->x] = VOXEL_TRAP;
            send_text_to_client(c->sock,
                                "[SYSTEM] You notice something suspicious on the floor...");
        }
    }
}

/*A dart trap shoots from its wall tile at the floor tile in front of it.*/
static void trap_dart_wall(Game *game, Client *c, Trap *t) {
    int wall_dx[4] = {0, 1, 0, -1};
    int wall_dy[4] = {-1, 0, 1, 0};
    int fire_x = t->x + wall_dx[t->wall_dir];
    int fire_y = t->y + wall_dy[t->wall_dir];
    static const char *dir_names[4] = {"north", "east", "south", "west"};
    if (c->x == fire_x && c->y == fire_y) {
        send_text_to_client(c->sock, "[DANGER] CLACK! A poison dart shoots out of the wall at %s!",
                            dir_names[t->wall_dir]);
        bool saved = player_save(c, ABILITY_DEX, t->save_dc, SAVE_PLAIN, NULL);
        if (saved) {
            send_text_to_client(c->sock, "[SYSTEM] You throw yourself to the side! The dart grazes"
                                         " your ear and embeds itself in the wall.");
        } else {
            int dmg_pierce =
                player_damage(c, rules_roll_dice(t->damage_dice, t->damage_sides), DMG_PIERCING);
            send_text_to_client(c->sock, "[COMBAT] The dart hits you! %d piercing damage.",
                                dmg_pierce);
            //Secondary CON save vs poison
            int poison_dc = t->save_dc - 2;
            bool saved_poison = player_save(c, ABILITY_CON, poison_dc, SAVE_PLAIN, NULL);
            if (!saved_poison && player_add_condition(c, COND_POISONED, 4, 0)) {
                send_text_to_client(
                    c->sock, "[STATE] The poison on the dart enters your blood!"
                             "You feel your strength abandon you... (Poisoned for 4 rounds)");
            } else if (saved_poison) {
                send_text_to_client(c->sock,
                                    "[STATE] The poison burns, but your constitution holds!");
            }
            if (c->hp <= 0) {
                c->hp = 0;
                send_text_to_client(c->sock, "[DEATH] The poison reaches your heart. You fall...");
                clog_death(c->username, "Poisoned dart", c->floor_id);
                save_bones(game, c);
                /*player_respawn_town() also tells the floor the player left
         * and re-syncs the client.*/
                player_respawn_town(game, c);
            }
        }
        t->active = false;
        t->respawn_timer = RESPAWN_TRAPS_TICKS;
        t->detected = false;
    }
}

/*A spring spear trap strikes the floor tile in front of its wall tile.*/
static void trap_spring_spear(Game *game, Client *c, Trap *t) {
    int wall_dx[4] = {0, 1, 0, -1};
    int wall_dy[4] = {-1, 0, 1, 0};
    static const char *dn[4] = {"north", "east", "south", "west"};
    int fire_x = t->x + wall_dx[t->wall_dir];
    int fire_y = t->y + wall_dy[t->wall_dir];
    if (c->x == fire_x && c->y == fire_y) {
        send_text_to_client(c->sock,
                            "[DANGER] SHHKK! A spring-loaded spear pops out of the wall at %s!",
                            dn[t->wall_dir]);
        bool sv = player_save(c, ABILITY_DEX, t->save_dc, SAVE_PLAIN, NULL);
        if (sv) {
            send_text_to_client(c->sock,
                                "[SYSTEM] Throw yourself to the ground! The spear grazes you!");
        } else {
            int dmg =
                player_damage(c, rules_roll_dice(t->damage_dice, t->damage_sides), DMG_PIERCING);
            send_text_to_client(c->sock, "[COMBAT] The spear pierces you! %d piercing damage.",
                                dmg);
            if (player_add_condition(c, COND_RESTRAINED, 2, 0)) {
                send_text_to_client(c->sock, "[STATE] You're stuck on the wall! (Restrained 2r)");
            }
            if (c->hp <= 0) {
                c->hp = 0;
                send_text_to_client(c->sock, "[DEATH] Pierced by the spear, you have fallen.");
                clog_death(c->username, "Spring spear", c->floor_id);
                save_bones(game, c);
                /*As for the dart wall: player_respawn_town() does it all.*/
                player_respawn_town(game, c);
            }
        }
        t->active = false;
        t->respawn_timer = RESPAWN_TRAPS_TICKS;
        t->detected = false;
    }
}

/*What a floor trap asks and deals: the saving throw (ABILITY_NONE: none),
 * whether it is a gas (an item may ward it) or magic (a rune), the type of
 * its damage, and the condition it leaves (MAX_CONDITIONS: none) with its
 * rounds and the message.*/
typedef struct {
    TrapType type;
    Ability save;
    SaveSource source;
    int damage_type;
    ConditionType cond;
    int rounds;
    const char *msg;
} TrapRule;

static const TrapRule k_trap_rules[] = {
    {TRAP_PIT, ABILITY_DEX, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_SPIKES, ABILITY_DEX, SAVE_PLAIN, DMG_PIERCING, MAX_CONDITIONS, 0, NULL},
    {TRAP_POISON_NEEDLE, ABILITY_CON, SAVE_PLAIN, DMG_PIERCING, COND_POISONED, 5,
     "[STATE] You are poisoned!"},
    {TRAP_GAS, ABILITY_CON, SAVE_GAS, DMG_POISON, COND_POISONED, 5, "[STATE] You are poisoned!"},
    {TRAP_ACID, ABILITY_NONE, SAVE_PLAIN, DMG_ACID, MAX_CONDITIONS, 0, NULL},
    {TRAP_WEB, ABILITY_DEX, SAVE_PLAIN, DAMAGE_UNTYPED, COND_RESTRAINED, 3,
     "[STATE] You are held by the spider web!"},
    {TRAP_BLADE, ABILITY_DEX, SAVE_PLAIN, DMG_SLASHING, COND_BLEEDING, 3,
     "[STATE] You're bleeding!"},
    {TRAP_SILENCE, ABILITY_CON, SAVE_MAGIC, DAMAGE_UNTYPED, COND_SILENCED, 2,
     "[STATE] You are surrounded by magical silence!"},
    {TRAP_LIGHTNING, ABILITY_CON, SAVE_PLAIN, DMG_LIGHTNING, MAX_CONDITIONS, 0, NULL},
    {TRAP_QUICKSAND, ABILITY_STR, SAVE_PLAIN, DAMAGE_UNTYPED, COND_PRONE, 1,
     "[STATE] You've landed in quicksand!"},
    {TRAP_DARKNESS, ABILITY_CON, SAVE_MAGIC, DAMAGE_UNTYPED, COND_BLINDED, 2,
     "[STATUS] You are blinded by darkness!"},
    {TRAP_FALLING_FLOOR, ABILITY_DEX, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_BOULDER, ABILITY_DEX, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_PARALYZING_SPORES, ABILITY_CON, SAVE_GAS, DMG_POISON, COND_PARALYZED, 2,
     "[STATUS] The spores paralyze you completely!"},
    {TRAP_CURSE_RUNE, ABILITY_WIS, SAVE_MAGIC, DMG_NECROTIC, MAX_CONDITIONS, 0, NULL},
    {TRAP_ANTIMAGIC_ZONE, ABILITY_NONE, SAVE_PLAIN, DMG_FORCE, COND_SILENCED, 5,
     "[STATUS] The magic leaves you (Silenced for 5 rounds)!"},
    {TRAP_SLEEP_GAS, ABILITY_CON, SAVE_GAS, DAMAGE_UNTYPED, COND_UNCONSCIOUS, 3,
     "[STATE] The gas makes you lose consciousness..."},
    {TRAP_ILLUSION_MIRROR, ABILITY_WIS, SAVE_MAGIC, DMG_PSYCHIC, MAX_CONDITIONS, 0, NULL},
    {TRAP_FAKE_DOOR, ABILITY_INT, SAVE_MAGIC, DAMAGE_UNTYPED, MAX_CONDITIONS, 0, NULL},
    {TRAP_FLOOD, ABILITY_DEX, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_CEILING_COLLAPSE, ABILITY_DEX, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_CRUSHER_WALL, ABILITY_STR, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_GAS_VEIN, ABILITY_CON, SAVE_GAS, DMG_FIRE, COND_POISONED, 5, "[STATE] You are poisoned!"},
    {TRAP_CRYSTAL_BURST, ABILITY_DEX, SAVE_PLAIN, DMG_PIERCING, MAX_CONDITIONS, 0, NULL},
    {TRAP_INVERSION_RUNE, ABILITY_WIS, SAVE_MAGIC, DMG_PSYCHIC, MAX_CONDITIONS, 0, NULL},
    {TRAP_STONE_TOMB, ABILITY_STR, SAVE_PLAIN, DMG_BLUDGEONING, MAX_CONDITIONS, 0, NULL},
    {TRAP_HUNGER_CURSE, ABILITY_CON, SAVE_MAGIC, DMG_NECROTIC, COND_BLEEDING, 5,
     "[STATE] A dark hunger slowly drains your life..."},
};

static const TrapRule *trap_rule(TrapType type) {
    for (size_t i = 0; i < sizeof(k_trap_rules) / sizeof(k_trap_rules[0]); i++) {
        if (k_trap_rules[i].type == type) {
            return &k_trap_rules[i];
        }
    }
    return NULL;
}

/*The saving throw of the ability the trap targets (none: not saved), with
 * the advantage of the items against gas or magic.*/
static bool trap_saving_throw(Client *c, const Trap *t) {
    const TrapRule *r = trap_rule(t->type);
    if (!r || r->save == ABILITY_NONE) {
        return false;
    }
    return player_save(c, r->save, t->save_dc, r->source, NULL);
}

/*Damage of a trap that was not saved, of its type (a ring of feather
 * falling spares a fall), and the condition it inflicts.*/
static void trap_damage(Game *game, Client *c, const Trap *t) {
    const TrapRule *r = trap_rule(t->type);
    int dmg = rules_roll_dice(t->damage_dice, t->damage_sides);

    //Special effects traps
    if (t->type == TRAP_LIGHTNING) {
        //Extra damage if in water
        Floor *fl = &game->world->floors[c->floor_id];
        if (fl->map.data[0][c->y][c->x] == VOXEL_WATER) {
            dmg *= 2;
        }
    }
    WornTotals w;
    player_worn(c, &w);
    if (w.feather_fall && (t->type == TRAP_PIT || t->type == TRAP_FALLING_FLOOR)) {
        send_text_to_client(c->sock, "[MAGIC] You float down gently: the fall does not hurt.");
        dmg = 0;
    }
    dmg = player_damage(c, dmg, r ? r->damage_type : DAMAGE_UNTYPED);
    send_text_to_client(c->sock, "[COMBAT] Take %d trap damage!", dmg);

    //Application of environmental conditions and special effects
    if (r && r->cond < MAX_CONDITIONS) {
        if (player_add_condition(c, r->cond, r->rounds, 0)) {
            send_text_to_client(c->sock, "%s", r->msg);
        }
    } else if (t->type == TRAP_INVERSION_RUNE) {
        ActiveEffect confused = {"Confused", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
        if (rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &confused)) {
            send_text_to_client(c->sock, "[STATE] Your mind becomes dizzyingly confused!");
        }
    }
}

/*What a trap does to the world around the player: a collapsing floor,
 * a flood, a cave-in, a stone tomb, a swap with a nearby monster.*/
static void trap_world_effect(Client *c, Trap *t, Game *game) {
    if (t->type == TRAP_FALLING_FLOOR) {
        send_text_to_client(c->sock, "[DANGER] THE FLOOR COLLAPSES UNDER YOUR FEET!");
        if (c->floor_id < MAX_FLOORS - 1) {
            int old_floor = c->floor_id;
            if (game->world->floors[old_floor].entity_grid[c->y][c->x] == c->entity_id) {
                game->world->floors[old_floor].entity_grid[c->y][c->x] = 0;
            }
            c->floor_id++;
            notify_player_left_floor(game, c, old_floor);
            /*Bounded scatter onto a walkable tile inside the map, even next to
       * the map edge.*/
            Floor *nf = &game->world->floors[c->floor_id];
            int lx = c->x + (rand() % 5) - 2;
            int ly = c->y + (rand() % 5) - 2;
            find_walkable_near(nf, lx, ly, &c->x, &c->y);
            nf->entity_grid[c->y][c->x] = c->entity_id;
            client_track_explored_floor(c);
            /*Arrival broadcast AFTER the position is set*/
            broadcast_player_state(game, c);
        }
    } else if (t->type == TRAP_FLOOD) {
        send_text_to_client(c->sock, "[DANGER] Water gushes out of hidden valves!");
        Floor *fl = &game->world->floors[c->floor_id];
        for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                int nx = c->x + dx;
                int ny = c->y + dy;
                if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                    if (fl->map.data[0][ny][nx] == VOXEL_FLOOR) {
                        fl->map.data[0][ny][nx] = VOXEL_WATER;
                    }
                }
            }
        }
    } else if (t->type == TRAP_CEILING_COLLAPSE) {
        send_text_to_client(c->sock, "[DANGER] THE CEILING IS COLLAPSING!");
        Floor *fl = &game->world->floors[c->floor_id];
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int nx = c->x + dx;
                int ny = c->y + dy;
                if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                    if (fl->map.data[0][ny][nx] == VOXEL_FLOOR && (rand() % 2 == 0)) {
                        fl->map.data[0][ny][nx] = VOXEL_ROCK;
                    }
                }
            }
        }
    } else if (t->type == TRAP_STONE_TOMB) {
        send_text_to_client(c->sock,
                            "[DANGER] Thick stone walls rise from the ground, trapping you!");
        Floor *fl = &game->world->floors[c->floor_id];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                int nx = c->x + dx;
                int ny = c->y + dy;
                if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                    fl->map.data[0][ny][nx] = VOXEL_WALL;
                }
            }
        }
    } else if (t->type == TRAP_SWAP_TELEPORT || t->type == TRAP_BODY_SWAP) {
        //Find a nearby enemy (within 10 tiles) to trade
        NPC *target_npc = NULL;
        FloorIter walk = floor_iter(game, c->floor_id);
        for (int n; (n = floor_iter_next(&walk)) >= 0;) {
            /*A monster: not a gold pile, a chest, a merchant or a ghost
             * (swapping hit points with a gold pile, which has none, killed
             * the player).*/
            const NPC *cand = &game->npcs[n];
            bool monster = cand->template && cand->hp > 0 && !cand->is_ghost &&
                           cand->archetype != ARCH_GOLD && cand->archetype != ARCH_TREASURE &&
                           cand->archetype != ARCH_MERCHANT;
            if (monster && abs(cand->x - c->x) <= 10 && abs(cand->y - c->y) <= 10) {
                target_npc = &game->npcs[n];
                break;
            }
        }
        if (target_npc) {
            if (t->type == TRAP_SWAP_TELEPORT) {
                send_text_to_client(c->sock, "[MAGIC] A flash of light! You trade places with %s!",
                                    npc_name(target_npc));
                int tx = c->x;
                int ty = c->y;
                /*Keep entity_grid consistent with the swap: collisions read it
         * before the next grid sync.*/
                Floor *sfl = &game->world->floors[c->floor_id];
                if (sfl->entity_grid[ty][tx] == c->entity_id) {
                    sfl->entity_grid[ty][tx] = 0;
                }
                if (sfl->entity_grid[target_npc->y][target_npc->x] == target_npc->entity_id) {
                    sfl->entity_grid[target_npc->y][target_npc->x] = 0;
                }
                c->x = target_npc->x;
                c->y = target_npc->y;
                target_npc->x = tx;
                target_npc->y = ty;
                sfl->entity_grid[c->y][c->x] = c->entity_id;
                sfl->entity_grid[target_npc->y][target_npc->x] = target_npc->entity_id;
            } else {
                send_text_to_client(c->sock,
                                    "[MAGIC] A dark force envelops you! It exchanges"
                                    " your vitality with %s!",
                                    npc_name(target_npc));
                int thp = c->hp;
                c->hp = target_npc->hp;
                target_npc->hp = thp;
            }
        } else {
            send_text_to_client(c->sock, "[MAGIC] The trap activates but finds"
                                         " no target nearby.");
        }
    }
}

/*After it fired: teleport traps move the player to a walkable tile and
 * stay armed, the others disarm until they respawn.*/
static void trap_after_firing(Game *game, Client *c, Trap *t, Floor *f) {
    if (t->type == TRAP_TELEPORT || t->type == TRAP_FAKE_DOOR) {
        /*Random teleport, but only onto a walkable free tile: never into
     * rock, onto a staircase or onto an occupied cell.*/
        Floor *tfl = &game->world->floors[c->floor_id];
        int anchor_x = 1 + rand() % (MAP_WIDTH - 2);
        int anchor_y = 1 + rand() % (MAP_HEIGHT - 2);
        int dest_x = c->x, dest_y = c->y;
        find_walkable_near(tfl, anchor_x, anchor_y, &dest_x, &dest_y);
        if (tfl->entity_grid[c->y][c->x] == c->entity_id) {
            tfl->entity_grid[c->y][c->x] = 0;
        }
        c->x = dest_x;
        c->y = dest_y;
        tfl->entity_grid[c->y][c->x] = c->entity_id;
        send_text_to_client(c->sock, "[MAGIC] Space distorts, you have been teleported!");
    } else if (t->type != TRAP_ILLUSION_MIRROR && t->type != TRAP_SWAP_TELEPORT) {
        t->active = false;
    }
    t->respawn_timer = RESPAWN_TRAPS_TICKS;
    t->detected = false; // Reset detection for next time
    if (f->map.data[0][t->y][t->x] == VOXEL_TRAP) {
        f->map.data[0][t->y][t->x] = VOXEL_FLOOR;
    }
}

/*A floor trap under the player.*/
static void trap_fire(Client *c, Trap *t, Floor *f, Game *game) {
    send_text_to_client(c->sock, "[DANGER] You have activated a trap!");
    if (trap_saving_throw(c, t)) {
        send_text_to_client(c->sock, "[SYSTEM] You avoid the worst!");
    } else {
        trap_damage(game, c, t);
        trap_world_effect(c, t, game);
        if (c->hp <= 0) {
            clog_death(c->username, "Trap", c->floor_id);
            save_bones(game, c);
            player_respawn_town(game, c);
            send_text_to_client(
                c->sock,
                "[DEATH] You died from a trap... The Arcane has returned you to town without your equipment!");
        }
    }
    trap_after_firing(game, c, t, f);
}

void check_traps(Client *c, Game *game) {
    Floor *f = &game->world->floors[c->floor_id];
    for (int i = 0; i < f->trap_count; i++) {
        Trap *t = &f->traps[i];
        if (!t->active) {
            continue;
        }

        trap_detect(c, t, f);
        /*Wall traps fire at the tile in front of them, never under the player.*/
        if (t->type == TRAP_DART_WALL && t->active) {
            trap_dart_wall(game, c, t);
            continue;
        }
        if (t->type == TRAP_SPRING_SPEAR && t->active) {
            trap_spring_spear(game, c, t);
            continue;
        }
        if (c->x == t->x && c->y == t->y) {
            trap_fire(c, t, f, game);
        }
    }
}

static bool secret_open_side(VoxelType v) {
    return v != VOXEL_WALL && v != VOXEL_ROCK;
}

bool secret_passage_at(const Floor *f, int floor_id, int x, int y) {
    if (floor_id <= 0 || x < 2 || y < 2 || x >= MAP_WIDTH - 2 || y >= MAP_HEIGHT - 2) {
        return false;
    }
    const VoxelType(*m)[MAP_WIDTH] = f->map.data[0];
    if (m[y][x] != VOXEL_WALL) {
        return false;
    }
    /*Wall to the left and right with open ground above and below, or the
     * other way round: a passage through a straight wall.*/
    bool vertical = secret_open_side(m[y - 1][x]) && secret_open_side(m[y + 1][x]) &&
                    m[y][x - 1] == VOXEL_WALL && m[y][x + 1] == VOXEL_WALL;
    bool horizontal = secret_open_side(m[y][x - 1]) && secret_open_side(m[y][x + 1]) &&
                      m[y - 1][x] == VOXEL_WALL && m[y + 1][x] == VOXEL_WALL;
    if (!vertical && !horizontal) {
        return false;
    }
    unsigned int h = (unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u ^
                     (unsigned int)floor_id * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h % SECRET_PASSAGE_RARITY == 0;
}

int secret_passage_dc(int floor_id) {
    return 12 + floor_id / 10;
}

int secret_passages_reveal(Game *game, Client *c, int radius, int perception) {
    Floor *f = &game->world->floors[c->floor_id];
    int dc = secret_passage_dc(c->floor_id);
    int found = 0;
    for (int y = c->y - radius; y <= c->y + radius; y++) {
        for (int x = c->x - radius; x <= c->x + radius; x++) {
            if (!secret_passage_at(f, c->floor_id, x, y)) {
                continue;
            }
            if (perception >= 0 && perception < dc) {
                continue;
            }
            f->map.data[0][y][x] = VOXEL_DOOR;
            found++;
            if (perception >= 0) {
                send_text_to_client(
                    c->sock,
                    "[PERCEPTION] You have discovered a secret passage at (%d, %d)! (Roll: %d vs DC: %d)",
                    x, y, perception, dc);
            } else {
                send_text_to_client(c->sock, "  > A secret passage at (%d, %d).", x, y);
            }
        }
    }
    if (found > 0) {
        send_map_chunk(c->sock, &f->map, c->x, c->y, INITIAL_VIEW_RADIUS);
    }
    return found;
}
