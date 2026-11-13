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
 * player_store.c - A character to and from its save file, and the bones a dead one leaves.
 */

#include "alignment.h"

#include "ai.h"
#include "server_internal.h"

#include "player.h"
#include "server_log.h"
#include "login_worker.h"

void save_player_data(Client *c) {
    if (!c || !c->authenticated) {
        return;
    }
    char p[STATE_DIR_MAX + 64];
    if (!state_path(p, sizeof(p), "saves/%s.save", c->username)) {
        return;
    }
    SaveData sd;
    memset(&sd, 0, sizeof(sd));
    /*The Argon2id string computed when the character was created (random
   * salt and parameters are encoded in it): the plaintext is never kept.*/
    copy_str(sd.password_hash, c->password_hash, sizeof(sd.password_hash));
    sd.x = c->x;
    sd.y = c->y;
    sd.floor_id = c->floor_id;
    sd.race_id = c->race_id;
    sd.subrace_id = c->subrace_id;
    sd.class_id = c->class_id;
    sd.level = c->level;
    sd.xp = c->xp;
    sd.str = c->str;
    sd.dex = c->dex;
    sd.con = c->con;
    sd.intel = c->intel;
    sd.wis = c->wis;
    sd.cha = c->cha;
    sd.gold = c->gold;
    sd.hp = c->hp;
    sd.max_hp = c->max_hp;
    sd.alignment = c->alignment;
    memcpy(sd.backpack, c->backpack, sizeof(c->backpack));
    sd.backpack_count = c->backpack_count;
    memcpy(sd.belt, c->belt, sizeof(c->belt));
    sd.s_head = c->slot_head;
    sd.s_neck = c->slot_neck;
    sd.s_body = c->slot_body;
    sd.s_back = c->slot_back;
    sd.s_hand_r = c->slot_hand_r;
    sd.s_hand_l = c->slot_hand_l;
    sd.s_hands = c->slot_hands;
    sd.s_arm_r = c->slot_arm_r;
    sd.s_arm_l = c->slot_arm_l;
    sd.s_feet = c->slot_feet;
    memcpy(sd.s_rings, c->slot_rings, sizeof(c->slot_rings));
    memcpy(sd.spell_slots, c->spell_slots, sizeof(c->spell_slots));
    memcpy(sd.spell_slots_max, c->spell_slots_max, sizeof(c->spell_slots_max));
    /*--- Phase 3: Active Effects, Resources and Statistics ---
   * Effects are flattened to SavedEffect: ActiveEffect.name is a
   * pointer, so a raw memcpy would serialize a heap address into the
   * file (it "worked" only because the loader ignored it). The name
   * becomes its canonical ConditionType; a name that is not a known
   * condition round-trips as MAX_CONDITIONS (see SavedEffect).*/
    int n_saved_effects = 0;
    for (int i = 0; i < c->effect_count && i < MAX_EFFECTS_PER_ENTITY; i++) {
        const ActiveEffect *e = &c->effects[i];
        SavedEffect *se = &sd.effects[i];
        se->cond = (int)condition_from_name(e->name);
        se->trigger = (int)e->trigger;
        se->mod_type = (int)e->mod_type;
        se->value = e->value;
        se->duration_rounds = e->duration_rounds;
        se->is_persistent = e->is_persistent ? 1 : 0;
        n_saved_effects++;
    }
    sd.effect_count = n_saved_effects;
    sd.light_turns_left = c->light_turns_left;
    sd.hunger_level = c->hunger_level;
    sd.exhaustion_level = c->exhaustion_level;
    sd.total_kills = c->total_kills;
    sd.total_steps = c->total_steps;
    sd.max_floor_explored = c->max_floor_explored;
    /* --- Persistent grimoire --- */
    memcpy(sd.known_spells, c->known_spells, sizeof(c->known_spells));
    /*--- Phase 6: Boss flags ---*/
    sd.bosses_defeated = c->bosses_defeated;
    sd.unspent_stat_points = c->unspent_stat_points;
    /*--- Messaging: block list ---*/
    memcpy(sd.blocked_players, c->blocked_players, sizeof(c->blocked_players));
    sd.blocked_count = c->blocked_count;
    /*Encoded here, field by field (persist.c), into a versioned,
   * CRC-checked container; the writer thread stores it atomically (a
   * failure keeps the previous save and is logged there).*/
    bool ok = persist_write_player(p, &sd);
    auth_wipe(&sd, sizeof(sd));
    if (ok) {
        server_log("SAVE", "Saved: %s", c->username);
    }
}

/*Sanitize an ItemInstance loaded from a save file: template_idx must be
 * -1 (empty slot) or a valid index into item_database. A hostile or
 * corrupted value (e.g. 9999) would later be used as an item_database[]
 * index (OOB read), so invalid slots are dropped (treated as empty).*/
static void sanitize_item_instance(ItemInstance *it) {
    if (it->template_idx != -1 &&
        (it->template_idx < 0 || it->template_idx >= item_database_size)) {
        memset(it, 0, sizeof(*it));
        it->template_idx = -1;
    }
}

/*Floor, position, identity, level, abilities and gold. True if a field
 * was out of range.*/
static bool restore_identity(Client *c, const SaveData *sd) {
    bool corrupted = false;
    /*Floor: must be in [0, MAX_FLOORS), otherwise force to 0 (surface).*/
    if (sd->floor_id < 0 || sd->floor_id >= MAX_FLOORS) {
        c->floor_id = 0;
        corrupted = true;
    } else {
        c->floor_id = sd->floor_id;
    }

    /*Position: strictly inside the map, negative values included, or the
   * first action would write outside entity_grid. Fallback: town centre.*/
    c->x = (sd->x >= 0 && sd->x < MAP_WIDTH) ? sd->x : MAP_CENTER_X;
    c->y = (sd->y >= 0 && sd->y < MAP_HEIGHT) ? sd->y : MAP_CENTER_Y;
    if (c->x != sd->x || c->y != sd->y) {
        corrupted = true;
    }

    /*Identity: out-of-range enums would index the RACES/SUBRACES/CLASSES/
   * ALIGNMENTS tables out of bounds. Same fallbacks as the new-character
   * path.*/
    c->race_id = (sd->race_id >= 0 && sd->race_id < RACE_COUNT) ? sd->race_id : 0;
    c->subrace_id = (sd->subrace_id >= -1 && sd->subrace_id < SUBRACE_COUNT) ? sd->subrace_id : -1;
    c->class_id =
        (sd->class_id >= 0 && sd->class_id < CLASS_COUNT) ? sd->class_id : CLASS_BARBARIAN;
    c->alignment = (sd->alignment >= 0 && sd->alignment < ALIGN_COUNT) ? sd->alignment : 0;

    /*Progression: level is capped at 20 (check_level_up / XP table);
   * xp cannot be negative; stats go through the same clamp used on the
   * new-character path.*/
    c->level = sd->level;
    if (c->level < 1 || c->level > 20) {
        c->level = 1;
        corrupted = true;
    }
    c->xp = sd->xp;
    if (c->xp < 0) {
        c->xp = 0;
        corrupted = true;
    }
    c->str = clamp_stat(sd->str);
    c->dex = clamp_stat(sd->dex);
    c->con = clamp_stat(sd->con);
    c->intel = clamp_stat(sd->intel);
    c->wis = clamp_stat(sd->wis);
    c->cha = clamp_stat(sd->cha);
    c->gold = sd->gold;
    return corrupted;
}

/*True if a field was out of range.*/
static bool restore_hit_points(Client *c, const SaveData *sd) {
    bool corrupted = false;
    /*HP: max_hp must be positive; hp must be in [0, max_hp]. A negative
   * hp is a "dead" character that would come back full on the next
   * damage — a free heal via a modified save.*/
    c->max_hp = sd->max_hp;
    if (c->max_hp <= 0) {
        c->max_hp = 20 + rules_get_modifier(c->con); /*new-character default*/
        if (c->max_hp < 1) {
            c->max_hp = 1;
        }
        corrupted = true;
    }
    c->hp = sd->hp;
    if (c->hp < 0) {
        c->hp = 0;
        corrupted = true;
    }
    if (c->hp > c->max_hp) {
        c->hp = c->max_hp;
        corrupted = true;
    }
    return corrupted;
}

/*Backpack, belt, equipment and spell slots. True if a count was out of
 * range.*/
static bool restore_inventory(Client *c, const SaveData *sd) {
    bool corrupted = false;
    /*Inventory: the item arrays are copied wholesale, but the COUNT must
   * be in [0, MAX_BACKPACK]. MAX_BACKPACK itself is a legal full backpack
   * (every pickup checks for space before writing).*/
    memcpy(c->backpack, sd->backpack, sizeof(sd->backpack));
    c->backpack_count = sd->backpack_count;
    if (c->backpack_count < 0 || c->backpack_count > MAX_BACKPACK) {
        c->backpack_count = 0;
        corrupted = true;
    }
    memcpy(c->belt, sd->belt, sizeof(sd->belt));
    c->slot_head = sd->s_head;
    c->slot_neck = sd->s_neck;
    c->slot_body = sd->s_body;
    c->slot_back = sd->s_back;
    c->slot_hand_r = sd->s_hand_r;
    c->slot_hand_l = sd->s_hand_l;
    c->slot_hands = sd->s_hands;
    c->slot_arm_r = sd->s_arm_r;
    c->slot_arm_l = sd->s_arm_l;
    c->slot_feet = sd->s_feet;
    memcpy(c->slot_rings, sd->s_rings, sizeof(sd->s_rings));
    memcpy(c->spell_slots, sd->spell_slots, sizeof(sd->spell_slots));
    memcpy(c->spell_slots_max, sd->spell_slots_max, sizeof(sd->spell_slots_max));
    /*Every ItemInstance loaded from disk may carry a hostile template_idx
   * (used later as an item_database[] index): any slot that is neither
   * empty (-1) nor a valid database index is dropped.*/
    for (int i = 0; i < MAX_BACKPACK; i++) {
        sanitize_item_instance(&c->backpack[i]);
    }
    for (int i = 0; i < MAX_BELT; i++) {
        sanitize_item_instance(&c->belt[i]);
    }
    sanitize_item_instance(&c->slot_head);
    sanitize_item_instance(&c->slot_neck);
    sanitize_item_instance(&c->slot_body);
    sanitize_item_instance(&c->slot_back);
    sanitize_item_instance(&c->slot_hand_r);
    sanitize_item_instance(&c->slot_hand_l);
    sanitize_item_instance(&c->slot_hands);
    sanitize_item_instance(&c->slot_arm_r);
    sanitize_item_instance(&c->slot_arm_l);
    sanitize_item_instance(&c->slot_feet);
    for (int i = 0; i < 10; i++) {
        sanitize_item_instance(&c->slot_rings[i]);
    }
    /*Spell slots: negative values would defeat the `<= 0' / `> 0' spend
   * checks; clamp into [0, max].*/
    for (int i = 0; i < MAX_SPELL_LEVEL + 1; i++) {
        if (c->spell_slots_max[i] < 0) {
            c->spell_slots_max[i] = 0;
        }
        if (c->spell_slots[i] < 0) {
            c->spell_slots[i] = 0;
        }
        if (c->spell_slots[i] > c->spell_slots_max[i]) {
            c->spell_slots[i] = c->spell_slots_max[i];
        }
    }
    return corrupted;
}

/*The effects are saved as SavedEffect (plain data): the name round-trips
 * as its canonical ConditionType and is bound again to the table string.
 * Effects that are not conditions (spell buffs, trap marks) are transient
 * and are dropped by design. True if the count was out of range.*/
static bool restore_effects(Client *c, const SaveData *sd) {
    bool corrupted = false;
    c->effect_count = 0;
    if (sd->effect_count < 0 || sd->effect_count > MAX_EFFECTS_PER_ENTITY) {
        c->effect_count = 0;
        corrupted = true;
    } else {
        for (int i = 0; i < sd->effect_count; i++) {
            const SavedEffect *se = &sd->effects[i];
            if (se->cond < 0 || se->cond >= MAX_CONDITIONS) {
                continue; /*not a canonical condition: dropped by design*/
            }
            ActiveEffect *e = &c->effects[c->effect_count];
            e->name = condition_to_name((ConditionType)se->cond);
            e->trigger = (RuleEventType)se->trigger;
            e->mod_type = (ModifierType)se->mod_type;
            e->value = se->value;
            e->duration_rounds = se->duration_rounds;
            if (e->duration_rounds < 0) {
                e->duration_rounds = 0;
            }
            e->is_persistent = (se->is_persistent != 0);
            c->effect_count++;
        }
    }
    return corrupted;
}

/*Light, hunger, exhaustion, statistics, the grimoire, boss flags, stat
 * points and the block list, each clamped to its range.*/
static void restore_counters(Client *c, const SaveData *sd) {
    c->light_turns_left = sd->light_turns_left;
    if (c->light_turns_left < 0) {
        c->light_turns_left = 0;
    }
    c->hunger_level = sd->hunger_level;
    if (c->hunger_level < 0) {
        c->hunger_level = 0;
    } else if (c->hunger_level > HUNGER_MAX) {
        c->hunger_level = HUNGER_MAX;
    }
    c->exhaustion_level = sd->exhaustion_level;
    if (c->exhaustion_level < 0 || c->exhaustion_level > 6) {
        c->exhaustion_level = 0;
    }
    c->total_kills = sd->total_kills;
    if (c->total_kills < 0) {
        c->total_kills = 0;
    }
    c->total_steps = sd->total_steps;
    if (c->total_steps < 0) {
        c->total_steps = 0;
    }
    /*Innate transit magic: the deepest floor ever reached*/
    c->max_floor_explored = sd->max_floor_explored;
    if (c->max_floor_explored < 0 || c->max_floor_explored >= MAX_FLOORS) {
        c->max_floor_explored = 0;
    }
    if (c->floor_id > c->max_floor_explored) {
        c->max_floor_explored = c->floor_id;
    }
    /* --- Grimoire --- */
    memcpy(c->known_spells, sd->known_spells, sizeof(sd->known_spells));
    c->bosses_defeated = sd->bosses_defeated;
    c->unspent_stat_points = sd->unspent_stat_points;
    if (c->unspent_stat_points < 0) {
        c->unspent_stat_points = 0;
    } else if (c->unspent_stat_points > 60) { /*max: 19 level-ups * 3 points*/
        c->unspent_stat_points = 60;
    }
    /*The list of blocked players*/
    if (sd->blocked_count > 0 && sd->blocked_count <= MAX_BLOCKED_PLAYERS) {
        memcpy(c->blocked_players, sd->blocked_players, sizeof(sd->blocked_players));
        c->blocked_count = sd->blocked_count;
    } else {
        c->blocked_count = 0;
    }
}

/*Apply a verified save image to the client (login_finish, or
 * load_player_data below).
 *
 * A save file is untrusted input: saves are written atomically, but
 * anyone with write access to saves/ can edit them while the server is
 * down, and a disk failure can corrupt them. Its fields are used as array
 * indices (floors[f], entity_grid[y][x], backpack[n], item_database[i])
 * or in HP arithmetic, so everything is clamped to its valid range, with
 * a safe fallback.*/
void apply_player_save(Client *c, const SaveData *sd) {
    copy_str(c->password_hash, sd->password_hash, sizeof(c->password_hash));
    bool corrupted = restore_identity(c, sd);
    corrupted = restore_hit_points(c, sd) || corrupted;
    corrupted = restore_inventory(c, sd) || corrupted;
    corrupted = restore_effects(c, sd) || corrupted;
    restore_counters(c, sd);
    if (corrupted) {
        server_log("SAVE", "Corrupted/tampered fields in %s.save — state sanitized on load.",
                   c->username);
    }
    server_log("SAVE", "Loaded: %s (floor %d, HP %d/%d, gold %lu, effects %d)", c->username,
               c->floor_id, c->hp, c->max_hp, (unsigned long)c->gold, c->effect_count);
}

/*Synchronous load (tests and tools; a network login goes through the
 * login worker). Returns 1 if loaded, 0 if the character does not exist,
 * -1 if the password is incorrect, -2 if the file exists but is unreadable
 * (other format version, corrupt): the login is refused and the file is
 * kept intact.*/
int load_player_data(Client *c, const char *password) {
    if (!c || !password) {
        return 0;
    }
    LoginJob *job = calloc(1, sizeof(*job));
    if (!job) {
        return -2;
    }
    int rc = -2;
    copy_str(job->ml.username, c->username, sizeof(job->ml.username));
    copy_str(job->ml.password, password, sizeof(job->ml.password));
    if (state_path(job->save_path, sizeof(job->save_path), "saves/%s.save", c->username)) {
        login_job_run(job);
        log_login_outcome(job);
        switch (job->outcome) {
        case LOGIN_LOADED:
            apply_player_save(c, &job->sd);
            rc = 1;
            break;
        case LOGIN_NEW:
            rc = 0;
            break;
        case LOGIN_BAD_PASSWORD:
            rc = -1;
            break;
        default:
            rc = -2;
            break;
        }
    }
    login_job_free(job);
    return rc;
}

void save_bones(Game *game, Client *c) {
    if (c->floor_id <= 0) {
        return;
    }

    /*--- 1. Create the Tombstone BEFORE emptying your inventory ---*/
    tombstone_create(game, c);

    /*--- 2. Create in-world NPC ghost (live ghost) ---*/
    int ghost_slot = -1;
    if (game->npcs) {
        for (int i = 0; i < MAX_NPCS; i++) {
            if (!game->npcs[i].active) {
                ghost_slot = i;
                break;
            }
        }
    }

    if (ghost_slot >= 0) {
        NPC *g = &game->npcs[ghost_slot];
        memset(g, 0, sizeof(NPC));
        /*active is set at the END of the setup, through npc_set_active:
         * the floor caches need the final fields.*/
        g->is_ghost = true;
        g->archetype = ARCH_BOSS;
        g->entity_id = game->next_id++;
        g->floor_id = c->floor_id;
        g->x = c->x;
        g->y = c->y;
        g->spawn_x = c->x;
        g->spawn_y = c->y;
        g->respawn_timer = 0;
        g->hp = 250;
        g->max_hp = 250;
        g->template_idx = -1;
        g->template = NULL;
        /*Look for a Ghost/Wraith/Spirit template in the bestiary*/
        for (int i = 0; i < bestiary_size; i++) {
            if (strcasestr(bestiary_data[i].name, "Ghost") ||
                strcasestr(bestiary_data[i].name, "Wraith") ||
                strcasestr(bestiary_data[i].name, "Spirit")) {
                g->template = &bestiary_data[i];
                g->template_idx = i;
                break;
            }
        }
        if (!g->template && bestiary_size > 0) {
            g->template = &bestiary_data[0];
            g->template_idx = 0;
        }
        snprintf(g->custom_name, sizeof(g->custom_name), "Ghost of %s", c->username);
        g->gold_drop = 0; /*The gold is in the tombstone, not in the ghost*/
        /*The template is final: activate through npc_set_active, which
         * keeps the floor caches in step.*/
        npc_set_active(game, g, true);
        /*Register in the entity_grid*/
        game->world->floors[c->floor_id].entity_grid[c->y][c->x] = g->entity_id;
        /*The corpse must be visible in the very next broadcast (the
         * per-floor index is rebuilt on the next tick at the latest,
         * but the player is standing right here: do it now).*/
        floor_index_rebuild(game);
        ai_init_npc(g, g->custom_name, g->floor_id);
        server_log("BONES", "Ghost of %s spawned on floor %d (%d,%d)", c->username, c->floor_id,
                   c->x, c->y);
    }

    /*--- 3. Empty the player's inventory ---*/
    c->gold = 0;
    for (int i = 0; i < MAX_BACKPACK; i++) {
        c->backpack[i].template_idx = -1;
        c->backpack[i].stack_count = 0;
    }
    c->backpack_count = 0;
    for (int i = 0; i < MAX_BELT; i++) {
        c->belt[i].template_idx = -1;
        c->belt[i].stack_count = 0;
    }
    c->slot_hand_r.template_idx = -1;
    c->slot_body.template_idx = -1;
    c->slot_head.template_idx = -1;
    c->slot_hand_l.template_idx = -1;
    c->slot_neck.template_idx = -1;
    c->slot_back.template_idx = -1;
    c->slot_hands.template_idx = -1;
    c->slot_arm_r.template_idx = -1;
    c->slot_arm_l.template_idx = -1;
    c->slot_feet.template_idx = -1;
    for (int i = 0; i < 10; i++) {
        c->slot_rings[i].template_idx = -1;
    }
}
