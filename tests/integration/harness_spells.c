/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * In-process spell tests: every kind of effect through the real "cast"
 * command, and every spell of data/spells.json once (spell_every_effect).
 * The d20 is forced (rules_force_d20) so that hits, misses and saves are
 * decided by the test; the damage dice still roll, so damage is checked
 * against its range.
 */

#include "harness_spells.h"

#include "fixture.h"

#include "aoe.h"
#include "items.h"
#include "map.h"
#include "rules.h"
#include "server_commands.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_world.h"
#include "spell_effect.h"
#include "spell_rules.h"
#include "spells.h"
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            rules_force_d20(0);                                                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

enum { FLOOR = 1, CX = 50, CY = 50 };

static char g_out[1 << 16];
static size_t g_out_len;

/*The server's output is framed binary: search it as bytes.*/
static bool said(const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= g_out_len; i++) {
        if (memcmp(g_out + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

static int spell_index(const char *name) {
    for (int i = 0; i < spell_database_size; i++) {
        if (strcmp(spell_database[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int book_of(const char *spell) {
    for (int i = 0; i < item_database_size; i++) {
        const ItemTemplate *t = &item_database[i];
        for (int k = 0; t->category == ITEM_BOOK && k < t->book_spell_count; k++) {
            if (strcmp(t->book_spell_names[k], spell) == 0) {
                return i;
            }
        }
    }
    return -1;
}

static void open_ground(void) {
    Floor *fl = &g_game.world->floors[FLOOR];
    for (int y = CY - 40; y <= CY + 40; y++) {
        for (int x = CX - 40; x <= CX + 40; x++) {
            fl->map.data[0][y][x] = VOXEL_FLOOR;
            fl->entity_grid[y][x] = 0;
        }
    }
}

/*An empty floor and a level-5 caster of class cls in its middle, with
 * every slot and 18 in every ability.*/
static Client *caster(ClassType cls) {
    static bool made;
    Client *c = &g_test_clients[0];
    if (!made) {
        c = fx_player(0, "spellcaster");
        made = c != NULL;
        if (!c) {
            return NULL;
        }
    }
    memset(g_game.npcs, 0, sizeof(NPC) * MAX_NPCS);
    aoe_init_clouds(&g_game);
    open_ground();
    c->class_id = cls;
    c->level = 5;
    c->str = c->dex = c->con = c->intel = c->wis = c->cha = 18;
    c->hp = c->max_hp = 60;
    c->floor_id = FLOOR;
    c->x = CX;
    c->y = CY;
    c->effect_count = 0;
    c->backpack_count = 0;
    memset(c->known_spells, 0, sizeof(c->known_spells));
    for (int l = 0; l <= MAX_SPELL_LEVEL; l++) {
        c->spell_slots[l] = c->spell_slots_max[l] = 9;
    }
    g_game.world->floors[FLOOR].entity_grid[CY][CX] = c->entity_id;
    fx_drain(0);
    return c;
}

/*The caster knows the spell and carries its book.*/
static const SpellTemplate *learn(Client *c, const char *name) {
    int si = spell_index(name);
    int book = book_of(name);
    if (si < 0 || book < 0) {
        return NULL;
    }
    spell_known_set(c, si);
    ItemInstance *it = &c->backpack[c->backpack_count++];
    fx_reset_item(it);
    it->template_idx = book;
    it->stack_count = 1;
    it->is_identified = true;
    return &spell_database[si];
}

static NPC *foe(int slot, int x, int y) {
    NPC *n = &g_game.npcs[slot];
    memset(n, 0, sizeof(*n));
    n->archetype = ARCH_MELEE;
    n->x = n->spawn_x = x;
    n->y = n->spawn_y = y;
    n->hp = n->max_hp = 10000;
    n->ac = 1;
    n->floor_id = FLOOR;
    n->entity_id = 9000 + slot;
    n->respawn_timer = -1;
    n->ai_ctx.current_target_id = -1;
    snprintf(n->custom_name, sizeof(n->custom_name), "Dummy %d", slot);
    npc_set_active(&g_game, n, true);
    g_game.world->floors[FLOOR].entity_grid[y][x] = n->entity_id;
    return n;
}

/*Casts; said() then searches what the caster read.*/
static const char *cast(Client *c, const char *name) {
    char line[128];
    snprintf(line, sizeof(line), "cast %s", name);
    fx_drain(0);
    handle_text_cmd(c, line, &g_game);
    g_out_len = fx_output(0, g_out, sizeof(g_out));
    return g_out;
}

static bool has_effect(const ActiveEffect *fx, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (fx[i].name && strcmp(fx[i].name, name) == 0 && fx[i].duration_rounds > 0) {
            return true;
        }
    }
    return false;
}

/* --- One test per kind of effect ------------------------------------------ */

/* Fire Bolt: an attack at range, scaled at level 5 (2d10), doubled on a
 * natural 20, nothing on a natural 1; a cantrip spends no slot. */
int test_spell_attack(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Fire Bolt"), "no caster or no Fire Bolt");
    NPC *n = foe(10, CX + 10, CY);
    n->ac = 15;
    rules_force_d20(10); /* 10 + 3 proficiency + 4 INT = 17 vs AC 15 */
    cast(c, "Fire Bolt");
    int dealt = n->max_hp - n->hp;
    CHECK(dealt >= 2 && dealt <= 20, "hit dealt %d, not 2d10", dealt);
    CHECK(said("attack 10+7 vs AC 15, hit"), "no attack line: %s", g_out);
    rules_force_d20(1);
    int hp = n->hp;
    cast(c, "Fire Bolt");
    CHECK(n->hp == hp && said("miss"), "a natural 1 hit");
    rules_force_d20(20);
    cast(c, "Fire Bolt");
    CHECK(hp - n->hp >= 4 && hp - n->hp <= 40, "critical dealt %d, not 4d10", hp - n->hp);
    CHECK(n->ai_ctx.current_target_id == c->entity_id, "the target did not turn on the caster");
    n->ac = 100;
    rules_force_d20(19);
    hp = n->hp;
    cast(c, "Fire Bolt");
    CHECK(n->hp == hp, "19 + 7 hit AC 100");
    rules_force_d20(0);
    return 0;
}

/* Fireball: centred on the nearest enemy (not on the caster), a Dexterity
 * save for half; the level-3 slot is spent. */
int test_spell_save_area(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Fireball"), "no caster or no Fireball");
    NPC *a = foe(10, CX + 8, CY);   /* the nearest: the centre */
    NPC *b = foe(11, CX + 11, CY);  /* 3 cells from a: inside */
    NPC *far = foe(12, CX, CY + 9); /* 12 cells from a: outside */
    rules_force_d20(2);             /* the saves fail */
    cast(c, "Fireball");
    int da = a->max_hp - a->hp, db = b->max_hp - b->hp;
    CHECK(da >= 8 && da <= 48 && db >= 8 && db <= 48, "failed saves: %d and %d, not 8d6", da, db);
    CHECK(far->hp == far->max_hp, "the creature outside the area was hit");
    CHECK(c->spell_slots[3] == 8, "slot: %d left", c->spell_slots[3]);
    CHECK(said("2 creatures caught"), "no summary: %s", g_out);
    rules_force_d20(20); /* the saves succeed: half */
    int hp = a->hp;
    cast(c, "Fireball");
    CHECK(hp - a->hp >= 4 && hp - a->hp <= 24, "successful save: %d, not half of 8d6", hp - a->hp);
    rules_force_d20(0);
    return 0;
}

/* Burning Hands and Lightning Bolt: from the caster towards the enemy. */
int test_spell_cone_and_line(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Burning Hands") && learn(c, "Lightning Bolt"), "missing spells");
    NPC *ahead = foe(10, CX + 2, CY);
    NPC *behind = foe(11, CX - 3, CY);
    rules_force_d20(2);
    cast(c, "Burning Hands");
    CHECK(ahead->hp < ahead->max_hp, "the cone missed the creature ahead");
    CHECK(behind->hp == behind->max_hp, "the cone hit the creature behind");
    NPC *in_line = foe(12, CX + 15, CY);
    NPC *aside = foe(13, CX + 8, CY + 4);
    cast(c, "Lightning Bolt");
    CHECK(in_line->hp < in_line->max_hp, "the line missed the creature 15 cells ahead");
    CHECK(aside->hp == aside->max_hp, "the line hit a creature 4 cells aside");
    rules_force_d20(0);
    return 0;
}

/* Hold Person: a failed Wisdom save paralyses for the effect's rounds,
 * and a paralysed NPC loses its turns; Thunderous Smite: the prone rider
 * of a hit needs a failed Strength save. */
int test_spell_conditions(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Hold Person"), "no Hold Person");
    NPC *n = foe(10, CX + 3, CY);
    rules_force_d20(20);
    cast(c, "Hold Person");
    CHECK(!rules_has_condition_t(n->effects, n->effect_count, COND_PARALYZED), "saved, yet held");
    CHECK(said("resisted"), "no save line: %s", g_out);
    rules_force_d20(2);
    cast(c, "Hold Person");
    CHECK(rules_has_condition_t(n->effects, n->effect_count, COND_PARALYZED), "failed, not held");
    CHECK(n->effects[0].duration_rounds == 10, "held %d rounds", n->effects[0].duration_rounds);

    c = caster(CLASS_PALADIN);
    CHECK(c && learn(c, "Thunderous Smite"), "no Thunderous Smite");
    n = foe(10, CX + 1, CY);
    rules_force_d20(18); /* hit; rider save 18 + 2 = 20 vs DC 15: resisted */
    cast(c, "Thunderous Smite");
    CHECK(n->hp < n->max_hp, "the smite missed");
    CHECK(!rules_has_condition_t(n->effects, n->effect_count, COND_PRONE),
          "prone despite the save");
    rules_force_d20(5); /* hit (AC 1); rider save 5 + 2 vs 15: prone */
    cast(c, "Thunderous Smite");
    CHECK(rules_has_condition_t(n->effects, n->effect_count, COND_PRONE), "not prone");
    rules_force_d20(0);
    return 0;
}

/* Cure Wounds heals the caster (1d8 + WIS, capped); Mage Armor raises the
 * AC by 3 and Bless the attack bonus, and casting again only refreshes. */
int test_spell_self(void) {
    Client *c = caster(CLASS_CLERIC);
    CHECK(c && learn(c, "Cure Wounds") && learn(c, "Bless"), "missing spells");
    c->hp = 10;
    cast(c, "Cure Wounds");
    CHECK(c->hp >= 15 && c->hp <= 22, "healed to %d, not 10 + 1d8 + 4", c->hp);
    c->hp = c->max_hp - 1;
    cast(c, "Cure Wounds");
    CHECK(c->hp == c->max_hp, "healed past the maximum: %d", c->hp);
    cast(c, "Bless");
    cast(c, "Bless");
    CHECK(c->effect_count == 1, "Bless twice: %d effects", c->effect_count);
    CHECK(rules_event_bonus(c->effects, c->effect_count, EVENT_ON_ATTACK) == 2, "no +2 to attack");

    c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Mage Armor"), "no Mage Armor");
    int ac = get_player_ac(c);
    cast(c, "Mage Armor");
    CHECK(get_player_ac(c) == ac + 3, "AC %d, was %d", get_player_ac(c), ac);
    return 0;
}

/* Cloud of Daggers stays: it cuts at the cast and every round after, for
 * its 3 rounds. */
int test_spell_cloud(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Cloud of Daggers"), "no Cloud of Daggers");
    NPC *n = foe(10, CX + 5, CY);
    cast(c, "Cloud of Daggers");
    int first = n->max_hp - n->hp;
    CHECK(first >= 4 && first <= 16, "first round %d, not 4d4", first);
    int clouds = 0;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        clouds += g_game.clouds[i].active;
    }
    CHECK(clouds == 1 && g_game.clouds[0].rounds_left == 2, "clouds %d, rounds left %d", clouds,
          g_game.clouds[0].rounds_left);
    for (int r = 0; r < 2; r++) {
        int hp = n->hp;
        aoe_update_clouds(&g_game, g_test_clients, MAX_CLIENTS);
        CHECK(n->hp < hp, "round %d of the cloud did nothing", r + 2);
    }
    CHECK(!g_game.clouds[0].active, "the cloud outlived its rounds");
    return 0;
}

/* Blink (a handler) moves the caster; Ice Storm damages AND ices the
 * ground (its terrain used to replace the damage). */
int test_spell_special_and_terrain(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Blink") && learn(c, "Ice Storm"), "missing spells");
    cast(c, "Blink");
    CHECK(c->x != CX || c->y != CY, "Blink did not move the caster");
    g_game.world->floors[FLOOR].entity_grid[c->y][c->x] = 0;
    c->x = CX;
    c->y = CY;
    NPC *n = foe(10, CX + 10, CY);
    rules_force_d20(2);
    cast(c, "Ice Storm");
    CHECK(n->hp < n->max_hp, "Ice Storm dealt no damage");
    CHECK(g_game.world->floors[FLOOR].map.data[0][CY][CX + 11] == VOXEL_ICE,
          "no ice by the target");
    rules_force_d20(0);
    return 0;
}

/* No enemy in range: nothing happens and the slot stays; a utility spell
 * goes off (slot spent) and says it does nothing in combat; an
 * incapacitated caster cannot cast. */
int test_spell_refusals(void) {
    Client *c = caster(CLASS_WIZARD);
    CHECK(c && learn(c, "Magic Missile") && learn(c, "Find Familiar"), "missing spells");
    cast(c, "Magic Missile");
    CHECK(said("no enemy within 24 cells"), "no refusal: %s", g_out);
    CHECK(c->spell_slots[1] == 9, "slot spent without a target");
    cast(c, "Find Familiar");
    CHECK(said("no effect in combat") && c->spell_slots[1] == 8, "utility: %s", g_out);
    ActiveEffect fx = {0};
    fx.name = condition_to_name(COND_INCAPACITATED);
    fx.duration_rounds = 2;
    c->effects[c->effect_count++] = fx;
    foe(10, CX + 3, CY);
    cast(c, "Magic Missile");
    CHECK(said("cannot act") && c->spell_slots[1] == 8, "cast while incapacitated");
    return 0;
}

/* --- Every spell of the data ---------------------------------------------- */

static ClassType class_of(const SpellTemplate *sp) {
    for (int k = 0; k < CLASS_COUNT; k++) {
        if (sp->class_mask & (1u << k)) {
            return (ClassType)k;
        }
    }
    return CLASS_WIZARD;
}

/*What a cast of sp must have done, by kind; NULL if it did.*/
static const char *check_outcome(const SpellTemplate *sp, Client *c, const NPC *n, int hp_before) {
    const SpellEffect *e = &sp->effect;
    switch (e->kind) {
    case SPELL_KIND_DAMAGE:
        if (n->hp >= n->max_hp) {
            return "no damage";
        }
        break;
    case SPELL_KIND_HEAL:
        return c->hp > hp_before ? NULL : "no healing";
    case SPELL_KIND_BUFF:
        return has_effect(c->effects, c->effect_count, sp->name) ? NULL : "no buff";
    case SPELL_KIND_UTILITY:
        return said("no effect in combat") ? NULL : "no utility line";
    case SPELL_KIND_SPECIAL:
        return said(sp->name) || said("[MAGIC]") ? NULL : "the handler said nothing";
    default:
        break;
    }
    if (e->condition < MAX_CONDITIONS &&
        !rules_has_condition_t((ActiveEffect *)n->effects, n->effect_count, e->condition)) {
        return "no condition";
    }
    return NULL;
}

/* Each of the spells, cast once at a creature one cell away with the d20
 * forced to 2 (attacks hit AC 1, saves fail): the effect its record
 * describes must happen. */
int test_spell_every_effect(void) {
    rules_force_d20(2);
    int failures = 0;
    for (int i = 0; i < spell_database_size; i++) {
        const SpellTemplate *sp = &spell_database[i];
        Client *c = caster(class_of(sp));
        CHECK(c && learn(c, sp->name), "%s: no book", sp->name);
        c->hp = 1;
        NPC *n = foe(10, CX + 1, CY);
        cast(c, sp->name);
        const char *why = check_outcome(sp, c, n, 1);
        if (why) {
            fprintf(stderr, "%s (%s, %s): %s\n", sp->name, spell_kind_name(sp->effect.kind),
                    spell_target_name(sp->effect.target), why);
            failures++;
        }
    }
    rules_force_d20(0);
    CHECK(failures == 0, "%d of %d spells did not do what their record says", failures,
          spell_database_size);
    return 0;
}
