/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * spell_utility.c - The spells of exploration, protection and support.
 *
 * Each handler reads its numbers from the record of the spell
 * (data/spells.json, "effect"): radius in cells, value (the parameter of
 * the handler), rounds, dice. Ranges are measured like the attacks: the
 * larger of the two axis distances.
 */

#include "spell_utility.h"

#include "ai.h"
#include "bestiary.h"
#include "items.h"
#include "map.h"
#include "player.h"
#include "server_internal.h"
#include "server_world.h"
#include "spell_cast.h"
#include "spell_rules.h"
#include "traps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Shared helpers -------------------------------------------------------- */

static int cells_between(int x1, int y1, int x2, int y2) {
    int dx = abs(x1 - x2);
    int dy = abs(y1 - y2);
    return dx > dy ? dx : dy;
}

/*"north-east" for a target up and to the right of (fx, fy); "here" on it.*/
static const char *compass(int fx, int fy, int tx, int ty) {
    static const char *const k_names[3][3] = {
        {"north-west", "north", "north-east"},
        {"west", "here", "east"},
        {"south-west", "south", "south-east"},
    };
    int dx = tx - fx, dy = ty - fy;
    /*A direction counts when it is at least half of the other one.*/
    int col = 1, row = 1;
    if (2 * abs(dx) >= abs(dy) && dx != 0) {
        col = dx > 0 ? 2 : 0;
    }
    if (2 * abs(dy) >= abs(dx) && dy != 0) {
        row = dy > 0 ? 2 : 0;
    }
    return k_names[row][col];
}

/*The hit points of the spell's dice, if it has any; 0 otherwise.*/
static int heal_from_dice(Client *c, const SpellTemplate *sp) {
    if (sp->effect.dice_count == 0 && sp->effect.dice_bonus == 0) {
        return 0;
    }
    SpellCaster sc;
    spell_caster_of(c, &sc);
    int amount = spell_roll_amount(&sp->effect, &sc, false);
    int before = c->hp;
    c->hp = c->hp + amount > c->max_hp ? c->max_hp : c->hp + amount;
    return c->hp - before;
}

static bool hostile_within(Game *game, const Client *c, int cells) {
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        const NPC *n = &game->npcs[i];
        if (n->floor_id == c->floor_id && spell_can_affect_npc(n) && n->hp > 0 &&
            cells_between(c->x, c->y, n->x, n->y) <= cells) {
            return true;
        }
    }
    return false;
}

static ItemInstance *worn_slot(Client *c, int i) {
    ItemInstance *slots[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                             &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                             &c->slot_arm_l,  &c->slot_feet};
    return i < 10 ? slots[i] : &c->slot_rings[i - 10];
}
#define WORN_SLOTS 20

/* --- Protection and care ------------------------------------------------- */

/*Every condition of the caster ends, but invisibility (which a spell
 * gave on purpose); the dice of the record, if any, heal too.*/
bool spell_cleanse(SpellContext *ctx) {
    Client *c = ctx->caster;
    int kept = 0, cured = 0;
    for (int i = 0; i < c->effect_count; i++) {
        ConditionType cond = condition_from_name(c->effects[i].name);
        if (cond < MAX_CONDITIONS && cond != COND_INVISIBLE) {
            cured++;
        } else {
            c->effects[kept++] = c->effects[i];
        }
    }
    c->effect_count = kept;
    int healed = heal_from_dice(c, ctx->sp);
    send_text_to_client(c->sock, "[MAGIC] %s: %d condition%s cured.", ctx->sp->name, cured,
                        cured == 1 ? "" : "s");
    if (healed > 0) {
        send_text_to_client(c->sock, "  > You recover %d HP (HP %d/%d).", healed, c->hp, c->max_hp);
    }
    send_detailed_state(ctx->game, c);
    return true;
}

/*The curses of the worn items lift, and the Cursed condition with them.*/
bool spell_remove_curse(SpellContext *ctx) {
    Client *c = ctx->caster;
    int lifted = 0;
    for (int i = 0; i < WORN_SLOTS; i++) {
        ItemInstance *it = worn_slot(c, i);
        if (it->template_idx != -1 && it->blessing == BLESS_CURSED) {
            it->blessing = BLESS_NORMAL;
            lifted++;
        }
    }
    for (int i = 0; i < c->effect_count; i++) {
        if (condition_from_name(c->effects[i].name) == COND_CURSED) {
            c->effects[i--] = c->effects[--c->effect_count];
            lifted++;
        }
    }
    send_text_to_client(c->sock, "[MAGIC] %s: %d curse%s lifted.", ctx->sp->name, lifted,
                        lifted == 1 ? "" : "s");
    send_detailed_state(ctx->game, c);
    return true;
}

/*Food and water: hunger is gone, a level of exhaustion too; the dice of
 * the record, if any, heal.*/
bool spell_nourish(SpellContext *ctx) {
    Client *c = ctx->caster;
    c->hunger_level = 0;
    if (c->exhaustion_level > 0) {
        c->exhaustion_level--;
    }
    int healed = heal_from_dice(c, ctx->sp);
    send_text_to_client(c->sock, "[MAGIC] %s: you eat and drink your fill.", ctx->sp->name);
    if (healed > 0) {
        send_text_to_client(c->sock, "  > You recover %d HP (HP %d/%d).", healed, c->hp, c->max_hp);
    }
    send_detailed_state(ctx->game, c);
    return true;
}

/*value tenths of its full durability back to the most worn item the
 * caster wears; restored in full it is no longer rusty.*/
bool spell_mend(SpellContext *ctx) {
    Client *c = ctx->caster;
    ItemInstance *worst = NULL;
    int worst_loss = 0;
    for (int i = 0; i < WORN_SLOTS; i++) {
        ItemInstance *it = worn_slot(c, i);
        if (it->template_idx < 0 || it->template_idx >= item_database_size) {
            continue;
        }
        int loss = item_database[it->template_idx].max_durability - it->durability;
        if (loss > worst_loss) {
            worst_loss = loss;
            worst = it;
        }
    }
    if (!worst) {
        send_text_to_client(c->sock, "[MAGIC] %s: nothing you wear needs mending.", ctx->sp->name);
        return true;
    }
    int max = item_database[worst->template_idx].max_durability;
    int value = ctx->sp->effect.value > 0 ? ctx->sp->effect.value : 1;
    int points = (max * value + 9) / 10;
    worst->durability = worst->durability + points > max ? max : worst->durability + points;
    if (worst->durability == max && worst->quality == QUALITY_RUSTY) {
        worst->quality = QUALITY_NORMAL;
    }
    char name[128];
    get_full_item_name(worst, name, sizeof(name));
    send_text_to_client(c->sock, "[MAGIC] %s: %s mended (durability %d/%d).", ctx->sp->name, name,
                        worst->durability, max);
    send_detailed_state(ctx->game, c);
    return true;
}

/*value unidentified items of the backpack are identified (0: all of
 * them); a curse is told.*/
bool spell_identify(SpellContext *ctx) {
    Client *c = ctx->caster;
    int left = ctx->sp->effect.value > 0 ? ctx->sp->effect.value : MAX_BACKPACK;
    int done = 0;
    for (int i = 0; i < c->backpack_count && left > 0; i++) {
        ItemInstance *it = &c->backpack[i];
        if (it->is_identified) {
            continue;
        }
        it->is_identified = true;
        left--;
        done++;
        char name[128];
        get_full_item_name(it, name, sizeof(name));
        send_text_to_client(c->sock, "  > [%d] %s%s", i + 1, name,
                            it->blessing == BLESS_CURSED ? " -- CURSED!" : "");
    }
    send_text_to_client(c->sock, "[MAGIC] %s: %d item%s identified.", ctx->sp->name, done,
                        done == 1 ? "" : "s");
    return true;
}

/*Sight of value cells in the dark for the rounds of the record (a torch
 * gives 6); the largest light counts, they do not add up.*/
bool spell_light(SpellContext *ctx) {
    Client *c = ctx->caster;
    const SpellEffect *e = &ctx->sp->effect;
    ActiveEffect fx;
    memset(&fx, 0, sizeof(fx));
    fx.name = ctx->sp->name;
    fx.trigger = EVENT_ON_LIGHT;
    fx.mod_type = MOD_ADDITIVE;
    fx.value = e->value > 0 ? e->value : 6;
    fx.duration_rounds = e->rounds > 0 ? e->rounds : 10;
    if (!rules_add_effect(c->effects, &c->effect_count, MAX_EFFECTS_PER_ENTITY, &fx)) {
        send_text_to_client(c->sock, "[MAGIC] You cannot sustain another magical effect.");
        return true;
    }
    send_text_to_client(c->sock, "[MAGIC] %s: you see %d cells around you for %d rounds.",
                        ctx->sp->name, fx.value, fx.duration_rounds);
    send_detailed_state(ctx->game, c);
    return true;
}

/*A rest away from danger: no enemy within 10 cells, and the hit points,
 * hunger and a level of exhaustion recover (the spell slots do not: only
 * the town's rest gives them back).*/
#define SHELTER_SAFE_CELLS 10
bool spell_shelter(SpellContext *ctx) {
    Client *c = ctx->caster;
    if (hostile_within(ctx->game, c, SHELTER_SAFE_CELLS)) {
        send_text_to_client(c->sock, "[MAGIC] %s: enemies are too close to rest (%d cells).",
                            ctx->sp->name, SHELTER_SAFE_CELLS);
        return true;
    }
    c->hp = c->max_hp;
    c->hunger_level = 0;
    if (c->exhaustion_level > 0) {
        c->exhaustion_level--;
    }
    send_text_to_client(c->sock, "[MAGIC] %s: a safe refuge. You rest (HP %d/%d).", ctx->sp->name,
                        c->hp, c->max_hp);
    send_detailed_state(ctx->game, c);
    return true;
}

/* --- Exploration ------------------------------------------------------------ */

/*The traps within radius are found and drawn on the map; with value 1
 * they are disarmed as well (no experience: the spell did the work).*/
bool spell_find_traps(SpellContext *ctx) {
    Client *c = ctx->caster;
    Floor *f = &ctx->game->world->floors[c->floor_id];
    int radius = ctx->sp->effect.radius > 0 ? ctx->sp->effect.radius : 1;
    bool disarm = ctx->sp->effect.value > 0;
    int found = 0;
    for (int i = 0; i < f->trap_count; i++) {
        Trap *t = &f->traps[i];
        if (!t->active || cells_between(c->x, c->y, t->x, t->y) > radius) {
            continue;
        }
        bool in_wall = t->type == TRAP_DART_WALL || t->type == TRAP_SPRING_SPEAR;
        found++;
        if (disarm) {
            t->active = false;
            t->detected = false;
            t->respawn_timer = RESPAWN_TRAPS_TICKS;
            if (f->map.data[0][t->y][t->x] == VOXEL_TRAP) {
                f->map.data[0][t->y][t->x] = VOXEL_FLOOR;
            }
        } else {
            t->detected = true;
            if (!in_wall) {
                f->map.data[0][t->y][t->x] = VOXEL_TRAP;
            }
        }
        send_text_to_client(c->sock, "  > A trap %d cells %s%s.",
                            cells_between(c->x, c->y, t->x, t->y), compass(c->x, c->y, t->x, t->y),
                            disarm ? ": disarmed" : "");
    }
    send_text_to_client(c->sock, "[MAGIC] %s: %d trap%s within %d cells%s.", ctx->sp->name, found,
                        found == 1 ? "" : "s", radius, disarm && found ? ", all disarmed" : "");
    return true;
}

static bool stairs_nearest(const Floor *f, VoxelType type, int x, int y, int *ox, int *oy) {
    int best = -1;
    for (int ty = 0; ty < MAP_HEIGHT; ty++) {
        for (int tx = 0; tx < MAP_WIDTH; tx++) {
            if (f->map.data[0][ty][tx] != type) {
                continue;
            }
            int d = cells_between(x, y, tx, ty);
            if (best < 0 || d < best) {
                best = d;
                *ox = tx;
                *oy = ty;
            }
        }
    }
    return best >= 0;
}

static void sense_stairs(Client *c, const Floor *f) {
    static const struct {
        VoxelType type;
        const char *what;
    } k_stairs[] = {{VOXEL_STAIRS_DOWN, "Stairs down"}, {VOXEL_STAIRS_UP, "Stairs up"}};
    for (size_t k = 0; k < sizeof(k_stairs) / sizeof(k_stairs[0]); k++) {
        int sx = 0, sy = 0;
        if (stairs_nearest(f, k_stairs[k].type, c->x, c->y, &sx, &sy)) {
            send_text_to_client(c->sock, "  > %s: %d cells %s.", k_stairs[k].what,
                                cells_between(c->x, c->y, sx, sy), compass(c->x, c->y, sx, sy));
        }
    }
}

/*The creatures and the things on the ground within radius (the nearest
 * twelve), and the way to the stairs of the floor.*/
#define SENSE_MAX_LINES 12
bool spell_sense(SpellContext *ctx) {
    Client *c = ctx->caster;
    Game *game = ctx->game;
    int radius = ctx->sp->effect.radius > 0 ? ctx->sp->effect.radius : 6;
    send_text_to_client(c->sock, "[MAGIC] %s: within %d cells you sense...", ctx->sp->name, radius);
    int shown = 0;
    for (int d = 0; d <= radius && shown < SENSE_MAX_LINES; d++) {
        FloorIter walk = floor_iter(game, c->floor_id);
        for (int i; (i = floor_iter_next(&walk)) >= 0 && shown < SENSE_MAX_LINES;) {
            const NPC *n = &game->npcs[i];
            if (n->floor_id != c->floor_id || cells_between(c->x, c->y, n->x, n->y) != d) {
                continue;
            }
            const char *where = compass(c->x, c->y, n->x, n->y);
            if (n->archetype == ARCH_TREASURE || n->archetype == ARCH_GOLD || n->is_ghost) {
                send_text_to_client(c->sock, "  > %s: %d cells %s.",
                                    n->is_ghost ? "Remains" : "Something on the ground", d, where);
            } else if (n->archetype == ARCH_MERCHANT) {
                send_text_to_client(c->sock, "  > A merchant: %d cells %s.", d, where);
            } else {
                send_text_to_client(c->sock, "  > %s%s (HP %d/%d): %d cells %s.", npc_name(n),
                                    npc_is_ally(n) ? " [ally]" : "", n->hp, n->max_hp, d, where);
            }
            shown++;
        }
    }
    if (shown == 0) {
        send_text_to_client(c->sock, "  > Nothing stirs.");
    }
    sense_stairs(c, &game->world->floors[c->floor_id]);
    return true;
}

/*Wall and rock within radius open into floor: never the border of the
 * map, never the town.*/
bool spell_shape_earth(SpellContext *ctx) {
    Client *c = ctx->caster;
    if (c->floor_id == 0) {
        send_text_to_client(c->sock, "[MAGIC] %s: the wards of the town hold its stones fast.",
                            ctx->sp->name);
        return true;
    }
    Floor *f = &ctx->game->world->floors[c->floor_id];
    int radius = ctx->sp->effect.radius > 0 ? ctx->sp->effect.radius : 1;
    int opened = 0;
    for (int y = c->y - radius; y <= c->y + radius; y++) {
        for (int x = c->x - radius; x <= c->x + radius; x++) {
            if (x < 1 || y < 1 || x >= MAP_WIDTH - 1 || y >= MAP_HEIGHT - 1) {
                continue;
            }
            VoxelType v = f->map.data[0][y][x];
            if (v == VOXEL_WALL || v == VOXEL_ROCK) {
                f->map.data[0][y][x] = VOXEL_FLOOR;
                opened++;
            }
        }
    }
    send_text_to_client(c->sock, "[MAGIC] %s: the stone gives way (%d cells opened).",
                        ctx->sp->name, opened);
    return true;
}

/* --- Allies and companions ---------------------------------------------------- */

/*Where the members of the caster's party are; they learn where the
 * caster is. Without a party, who is in the game and on which floor.*/
bool spell_rally(SpellContext *ctx) {
    Client *c = ctx->caster;
    Game *game = ctx->game;
    bool party = c->party_leader[0] != '\0';
    int reached = 0;
    send_text_to_client(c->sock, "[MAGIC] %s: you reach out to %s...", ctx->sp->name,
                        party ? "your party" : "the minds in the dungeon");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &game->clients[i];
        if (!o->active || !o->authenticated || o == c) {
            continue;
        }
        if (party && strcmp(o->party_leader, c->party_leader) != 0) {
            continue;
        }
        reached++;
        if (party) {
            send_text_to_client(c->sock, "  > %s: floor %d (%d,%d), HP %d/%d.", o->username,
                                o->floor_id, o->x, o->y, o->hp, o->max_hp);
            send_text_to_client(o->sock, "[MAGIC] %s reaches your mind: floor %d (%d,%d).",
                                c->username, c->floor_id, c->x, c->y);
        } else {
            send_text_to_client(c->sock, "  > %s: floor %d.", o->username, o->floor_id);
        }
    }
    if (reached == 0) {
        send_text_to_client(c->sock, "  > No one answers.");
    }
    return true;
}

/*A creature of the floor's bestiary (the floor's own, the first floor's
 * in town), or -1.*/
static int summon_template(int floor_id) {
    int depth = floor_id > 0 ? floor_id : 1;
    int pool = 0, chosen = -1;
    for (int i = 0; i < bestiary_size; i++) {
        if (depth >= bestiary_data[i].floor_min && depth <= bestiary_data[i].floor_max) {
            /*Reservoir sampling: each one is equally likely.*/
            if (rules_roll_dice(1, ++pool) == 1) {
                chosen = i;
            }
        }
    }
    return chosen;
}

/*value allies (at least one) beside the caster, for the rounds of the
 * record: creatures of the floor's bestiary that fight for the caster
 * and never respawn.*/
bool spell_summon(SpellContext *ctx) {
    Client *c = ctx->caster;
    Game *game = ctx->game;
    int count = ctx->sp->effect.value > 0 ? ctx->sp->effect.value : 1;
    int rounds = ctx->sp->effect.rounds > 0 ? ctx->sp->effect.rounds : 20;
    int called = 0;
    for (int k = 0; k < count; k++) {
        int tidx = summon_template(c->floor_id);
        int sx = 0, sy = 0;
        if (tidx < 0 || !spell_find_spawn_cell(game, c, &sx, &sy)) {
            break;
        }
        NPC *n = npc_alloc(game);
        if (!n) {
            break;
        }
        memset(n, 0, sizeof(*n));
        n->entity_id = game->next_id++;
        n->template_idx = tidx;
        n->template = &bestiary_data[tidx];
        n->floor_id = c->floor_id;
        n->x = sx;
        n->y = sy;
        n->spawn_x = -1; /*no spawn point: it never respawns*/
        n->spawn_y = -1;
        ai_init_npc(n, n->template->name, n->floor_id);
        n->hp = n->max_hp;
        ai_make_ally(n, c, rounds);
        snprintf(n->custom_name, sizeof(n->custom_name), "[SUMMONED] %s", n->template->name);
        npc_set_active(game, n, true);
        game->world->floors[c->floor_id].entity_grid[sy][sx] = n->entity_id;
        send_text_to_client(c->sock, "  > %s (HP %d, AC %d) answers your call.", n->template->name,
                            n->hp, n->ac);
        called++;
    }
    if (called == 0) {
        send_text_to_client(c->sock, "[MAGIC] %s: no creature can answer (no free space).",
                            ctx->sp->name);
        return true;
    }
    send_text_to_client(c->sock, "[MAGIC] %s: %d all%s fight%s for you for %d rounds.",
                        ctx->sp->name, called, called == 1 ? "y" : "ies", called == 1 ? "s" : "",
                        rounds);
    return true;
}
