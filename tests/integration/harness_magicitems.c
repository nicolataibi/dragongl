/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Magic items do what their description says (see harness_magicitems.h).
 */

#include "harness_magicitems.h"

#include "harness_util.h"

#include "ai.h"
#include "inventory.h"
#include "item_effects.h"
#include "items.h"
#include "player.h"
#include "server_combat.h"
#include "server_internal.h"
#include "server_traps.h"
#include "server_world.h"
#include "spell_cast.h"
#include "traps.h"

#include <string.h>

enum { CX = HX_CX, CY = HX_CY, FLOOR = HX_FLOOR };

/* Puts the named item on: a copy in the backpack, then 'wear'. Returns
 * its template, -1 (after the message) when it is not in the data. */
static int put_on(Client *c, const char *name) {
    int t = item_named(name);
    if (t < 0) {
        fprintf(stderr, "no %s in the data\n", name);
        return -1;
    }
    give(c, t, 1);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "wear %d", c->backpack_count);
    run(c, cmd);
    return t;
}

/* The value of the first live effect with this trigger (0: none). */
static int effect_value(const Client *c, RuleEventType trigger) {
    for (int i = 0; i < c->effect_count; i++) {
        if (c->effects[i].trigger == trigger && c->effects[i].duration_rounds > 0) {
            return c->effects[i].value;
        }
    }
    return 0;
}

static void use_new(Client *c, const char *name) {
    c->backpack_count = 0;
    give(c, item_named(name), 1);
    run(c, "use 1");
}

/* Where each kind is worn, by name: a staff is a quarterstaff (the Staff of
 * Withering was a ring), an oil is no lamp oil, Ioun stones are carried. */
int test_magic_items_kinds(void) {
    static const struct {
        const char *name;
        ItemCategory cat;
    } k_kinds[] = {
        {"Staff of Withering", ITEM_WEAPON},
        {"Rod of Lordly Might", ITEM_WEAPON},
        {"Ioun Stone of Strength", ITEM_MISC},
        {"Headband of Intellect", ITEM_HEAD},
        {"Goggles of Night", ITEM_HEAD},
        {"Medallion of Thoughts", ITEM_NECK},
        {"Robe of Eyes", ITEM_ARMOR},
        {"Wings of Flying", ITEM_BACK},
        {"Oil of Sharpness", ITEM_CONSUMABLE},
        {"Lantern of Revealing", ITEM_LIGHT_SOURCE},
        {"Ring of Stupidity", ITEM_RING},
        {"Horseshoes of Speed", ITEM_MISC},
        {"Oil (flask)", ITEM_FUEL},
        {"Boots of Speed", ITEM_FEET},
    };
    for (size_t i = 0; i < sizeof(k_kinds) / sizeof(k_kinds[0]); i++) {
        int t = item_named(k_kinds[i].name);
        CHECK(t >= 0 && item_database[t].category == k_kinds[i].cat, "%s: category %d, not %d",
              k_kinds[i].name, t >= 0 ? (int)item_database[t].category : -1, (int)k_kinds[i].cat);
    }
    /* No bonus from a word of the name any more: the boots of speed give
     * speed, not DEX; the amulet of health sets CON, it does not add. */
    int boots = item_named("Boots of Speed"), amulet = item_named("Amulet of Health");
    CHECK(item_database[boots].dex_bonus == 0 && item_database[boots].worn.haste,
          "boots of speed: DEX %d", item_database[boots].dex_bonus);
    CHECK(item_database[amulet].con_bonus == 0 && item_database[amulet].worn.ability_set[2] == 19,
          "amulet of health: CON +%d", item_database[amulet].con_bonus);
    int heal = item_named("Potion of Supreme Healing");
    CHECK(item_database[heal].use.heal.count == 10 && item_database[heal].use.heal.bonus == 20,
          "supreme healing is not 10d4+20");
    ItemDice d = {0, 0, 0};
    CHECK(item_parse_dice("15", &d) && d.count == 0 && d.bonus == 15, "a plain number");
    CHECK(!item_parse_dice("2d", &d) && !item_parse_dice("x", &d) && !item_parse_dice("1d4+", &d),
          "bad dice accepted");
    return 0;
}

/* "Your Strength score is 19", "+2, to a maximum of 20", a carried Ioun
 * stone (one per kind, once identified), a cursed ring that takes. */
int test_magic_items_abilities(void) {
    hx_reset_world();
    Client *c = hero(0, "mighty", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    int ts, td, tc, ti, tw, th;
    CHECK(put_on(c, "Gauntlets of Ogre Power") >= 0, "no gauntlets");
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(ts == 19, "ogre power: STR %d", ts);
    c->str = 20;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(ts == 20, "the gauntlets lowered STR 20 to %d", ts);
    c->str = 14;

    int belt = item_named("Belt of Dwarvenkind");
    give(c, belt, 1);
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(tc == 16, "the belt carried: CON %d", tc);
    give(c, belt, 1);
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(tc == 16, "two belts: CON %d", tc);
    c->con = 19;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(tc == 20, "CON 19 + 2 capped at 20: %d", tc);
    c->con = 14;
    c->backpack_count = 0;

    c->slot_hands.template_idx = -1;
    give(c, item_named("Ioun Stone of Strength"), 1)->is_identified = false;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(ts == 14, "an unknown Ioun stone counted: STR %d", ts);
    c->backpack[0].is_identified = true;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(ts == 16, "the Ioun stone of strength: STR %d", ts);

    CHECK(put_on(c, "Ring of Stupidity") >= 0, "no ring of stupidity");
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    CHECK(ti == 12, "the cursed ring: INT %d", ti);
    return 0;
}

/* Armour class and saves: protection +1 to both, the bracers of defence
 * only without armour and shield, the robe of the archmagi's base 15 and
 * its spell DC; a luckstone carried. */
int test_magic_items_defence(void) {
    hx_reset_world();
    Client *c = hero(0, "warded", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    /* WIS: the barbarian has no proficiency in it. */
    CHECK(get_player_ac(c) == 12 && player_save_mod(c, ABILITY_WIS) == 2, "naked: AC %d",
          get_player_ac(c));
    put_on(c, "Cloak of Protection");
    CHECK(get_player_ac(c) == 13 && player_save_mod(c, ABILITY_WIS) == 3,
          "cloak of protection: AC %d, save %+d", get_player_ac(c),
          player_save_mod(c, ABILITY_WIS));
    put_on(c, "Bracers of Defense");
    CHECK(get_player_ac(c) == 15, "bracers of defense unarmoured: AC %d", get_player_ac(c));
    int shield = item_of(ITEM_SHIELD);
    c->slot_hand_l = *give(c, shield, 1);
    c->backpack_count--;
    CHECK(get_player_ac(c) == 13 + item_database[shield].ac_bonus, "with a shield: AC %d",
          get_player_ac(c));
    c->slot_hand_l.template_idx = -1;
    give(c, item_named("Luckstone"), 1);
    CHECK(player_save_mod(c, ABILITY_DEX) == 4, "luckstone: save %+d",
          player_save_mod(c, ABILITY_DEX));

    hero(0, "warded", FLOOR, CX, CY);
    put_on(c, "Robe of the Archmagi");
    CHECK(c->slot_body.template_idx >= 0 && get_player_ac(c) == 17,
          "robe of the archmagi: AC %d, not 15 + DEX", get_player_ac(c));
    SpellCaster sc;
    spell_caster_of(c, &sc);
    CHECK(sc.dc_bonus == 2 && sc.attack_bonus == 2, "the robe's spell DC +%d, attack +%d",
          sc.dc_bonus, sc.attack_bonus);
    return 0;
}

/* Resistance halves, immunity cancels, damage and conditions alike; a
 * ring of resistance has its own type; temporary hit points go first. */
int test_magic_items_resistance(void) {
    hx_reset_world();
    Client *c = hero(0, "resistant", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    put_on(c, "Ring of Warmth");
    CHECK(player_damage(c, 10, DMG_COLD) == 5 && c->hp == 55, "ring of warmth: cold HP %d", c->hp);
    CHECK(player_damage(c, 10, DMG_FIRE) == 10 && c->hp == 45, "ring of warmth resists fire");
    CHECK(player_damage(c, 0, DMG_FIRE) == 0, "nothing hurt");

    put_on(c, "Periapt of Proof Against Poison");
    c->hp = 60;
    CHECK(player_damage(c, 12, DMG_POISON) == 0 && c->hp == 60, "the periapt: poison hurt");
    CHECK(!player_add_condition(c, COND_POISONED, 5, 0) && c->effect_count == 0,
          "the periapt: poisoned");
    heard(c);
    CHECK(said("you are not Poisoned"), "the ward is not told");

    put_on(c, "Ring of Free Action");
    CHECK(!player_add_condition(c, COND_PARALYZED, 5, 0) &&
              !player_add_condition(c, COND_RESTRAINED, 5, 0) &&
              player_add_condition(c, COND_PRONE, 1, 0),
          "free action");

    hero(0, "resistant", FLOOR, CX, CY);
    put_on(c, "Ring of Resistance");
    WornTotals w;
    player_worn(c, &w);
    int kind = c->slot_rings[0].current_charges - 1;
    CHECK(kind >= 0 && kind < MAX_DAMAGE_TYPES && w.resist == (1u << kind),
          "ring of resistance: type %d, mask %#x", kind, (unsigned)w.resist);
    char name[128];
    get_full_item_name(&c->slot_rings[0], name, sizeof(name));
    CHECK(strstr(name, damage_type_to_name((DamageType)kind)) != NULL, "not named: %s", name);

    hero(0, "resistant", FLOOR, CX, CY);
    put_on(c, "Plate Armor of Invulnerability");
    CHECK(player_damage(c, 10, DAMAGE_PHYSICAL) == 5 && player_damage(c, 10, DMG_ACID) == 10 &&
              player_damage(c, 10, DAMAGE_UNTYPED) == 10,
          "invulnerability");
    put_on(c, "Dragon Scale Mail");
    player_worn(c, &w);
    int scale = c->slot_body.current_charges - 1;
    CHECK(scale == DMG_ACID || scale == DMG_COLD || scale == DMG_FIRE || scale == DMG_LIGHTNING ||
              scale == DMG_POISON,
          "dragon scale mail resists %d", scale);

    hero(0, "resistant", FLOOR, CX, CY);
    use_new(c, "Potion of Heroism");
    CHECK(effect_value(c, EVENT_ON_TEMP_HP) == 10 && effect_value(c, EVENT_ON_ATTACK) == 2 &&
              effect_value(c, EVENT_ON_SAVE) == 2 && player_save_mod(c, ABILITY_WIS) == 4,
          "heroism: temp %d, attack %d", effect_value(c, EVENT_ON_TEMP_HP),
          effect_value(c, EVENT_ON_ATTACK));
    player_damage(c, 4, DAMAGE_UNTYPED);
    CHECK(c->hp == 60 && effect_value(c, EVENT_ON_TEMP_HP) == 6, "temporary HP: %d", c->hp);
    player_damage(c, 10, DAMAGE_UNTYPED);
    CHECK(c->hp == 56 && effect_value(c, EVENT_ON_TEMP_HP) == 0, "temporary HP spent: %d", c->hp);
    return 0;
}

/* Potions and oils: the dice of healing, haste, a random resistance,
 * poison and its cure, an oil for a weapon, a spell in a bottle, and the
 * ones whose magic does nothing here (kept). */
int test_magic_items_potions(void) {
    hx_reset_world();
    Client *c = hero(0, "drinker", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    c->hp = 1;
    use_new(c, "Potion of Healing");
    CHECK(c->hp >= 5 && c->hp <= 11 && c->backpack_count == 0, "2d4+2: HP %d", c->hp);
    c->max_hp = 200;
    c->hp = 1;
    use_new(c, "Potion of Supreme Healing");
    CHECK(c->hp >= 31 && c->hp <= 61, "10d4+20: HP %d", c->hp);

    use_new(c, "Potion of Speed");
    CHECK(player_hasted(c) && get_movement_cooldown(c) < 0.15f && get_player_ac(c) == 14,
          "speed: hasted %d, AC %d", player_hasted(c), get_player_ac(c));
    use_new(c, "Potion of Resistance");
    uint32_t r = (uint32_t)effect_value(c, EVENT_ON_RESIST);
    CHECK(r != 0 && (r & (r - 1)) == 0, "resistance: mask %#x", (unsigned)r);
    use_new(c, "Potion of Diminution");
    CHECK(effect_value(c, EVENT_ON_DAMAGE) == -2, "diminution: damage %d",
          effect_value(c, EVENT_ON_DAMAGE));

    hero(0, "drinker", FLOOR, CX, CY);
    rules_force_d20(1);
    use_new(c, "Potion of Poison");
    rules_force_d20(0);
    CHECK(c->hp < 60 && rules_has_condition_t(c->effects, c->effect_count, COND_POISONED),
          "poison: HP %d", c->hp);
    c->exhaustion_level = 2;
    use_new(c, "Potion of Vitality");
    CHECK(!rules_has_condition_t(c->effects, c->effect_count, COND_POISONED) &&
              c->exhaustion_level == 0,
          "vitality did not cure");
    c->hp = 3;
    rules_force_d20(1);
    use_new(c, "Potion of Poison");
    rules_force_d20(0);
    CHECK(c->hp == 60 && c->floor_id == 0, "a deadly poison: HP %d, floor %d", c->hp, c->floor_id);

    hero(0, "drinker", FLOOR, CX, CY);
    use_new(c, "Oil of Sharpness");
    CHECK(said("must coat a weapon") && c->backpack_count == 1, "oil without a weapon");
    c->slot_hand_r = *give(c, item_of(ITEM_WEAPON), 1);
    c->backpack_count = 1;
    run(c, "use 1");
    CHECK(effect_value(c, EVENT_ON_ATTACK) == 3 && effect_value(c, EVENT_ON_DAMAGE) == 3 &&
              c->backpack_count == 0,
          "oil of sharpness");
    use_new(c, "Oil of Slipperiness");
    CHECK(!player_add_condition(c, COND_RESTRAINED, 5, 0), "slipperiness: restrained");
    use_new(c, "Potion of Invisibility");
    CHECK(rules_has_condition_t(c->effects, c->effect_count, COND_INVISIBLE), "not invisible");

    use_new(c, "Potion of Water Breathing");
    CHECK(said("no hold in the dungeon") && c->backpack_count == 1, "water breathing used up");
    use_new(c, "Potion of Mind Reading");
    CHECK(said("You drink Potion of Mind Reading: Detect Thoughts") && c->backpack_count == 0,
          "mind reading");
    return 0;
}

/* Spells from items: at will only while worn, charges spent by their cost,
 * beads that never come back, a ring of evasion, a staff that spends a
 * charge on a hit. */
int test_magic_items_charges(void) {
    hx_reset_world();
    Client *c = hero(0, "wielder", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    give(c, item_named("Helm of Comprehending Languages"), 1);
    run(c, "use 1");
    CHECK(said("Wear or hold") && c->backpack_count == 1, "an at-will helm from the backpack");
    run(c, "wear 1");
    run(c, "use helm");
    CHECK(said("You invoke Helm of Comprehending Languages") && c->slot_head.template_idx >= 0,
          "the helm at will");
    run(c, "use cloak");
    CHECK(said("Item not found"), "used a cloak not worn");
    put_on(c, "Ring of Warmth");
    run(c, "use ring");
    CHECK(said("no power to use"), "used a plain ring");

    put_on(c, "Medallion of Thoughts");
    run(c, "use medallion");
    CHECK(c->slot_neck.current_charges == 2, "medallion: %d charges", c->slot_neck.current_charges);
    c->slot_neck.current_charges = 0;
    run(c, "use medallion");
    CHECK(said("no charges left"), "a medallion without charges");

    NPC *m = monster(10, NULL, FLOOR, CX + 3, CY);
    m->hp = m->max_hp = 10000;
    put_on(c, "Staff of Fire");
    run(c, "use staff");
    CHECK(c->slot_hand_r.current_charges == 7, "staff of fire: %d charges (3 a fireball)",
          c->slot_hand_r.current_charges);

    put_on(c, "Necklace of Fireballs");
    c->slot_neck.current_charges = 2;
    c->slot_hand_r.current_charges = 0;
    place(c, 0, CX, CY);
    run(c, "rest");
    CHECK(c->slot_neck.current_charges == 2 && c->slot_hand_r.current_charges == 10,
          "rest: beads %d, staff %d", c->slot_neck.current_charges, c->slot_hand_r.current_charges);
    c->slot_neck.current_charges = 0;
    run(c, "use necklace");
    CHECK(said("is spent"), "spent beads");

    hero(0, "wielder", FLOOR, CX, CY);
    put_on(c, "Ring of Evasion");
    rules_force_d20(1);
    bool dex = player_save(c, ABILITY_DEX, 15, SAVE_PLAIN, NULL);
    bool con = player_save(c, ABILITY_CON, 15, SAVE_PLAIN, NULL);
    rules_force_d20(0);
    CHECK(dex && !con && c->slot_rings[0].current_charges == 2, "evasion: %d %d, %d charges", dex,
          con, c->slot_rings[0].current_charges);
    return 0;
}

/* Magic weapons: their own attack and damage, dice on a hit (with a
 * charge), dice on a critical (stolen life); a robe of eyes sees the
 * invisible. */
int test_magic_items_weapons(void) {
    hx_reset_world();
    Client *c = hero(0, "blade", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    NPC *m = monster(10, NULL, FLOOR, CX + 1, CY);
    m->hp = m->max_hp = 10000;
    put_on(c, "Scimitar of Speed");
    rules_force_d20(15);
    perform_attack(c, m, &g_game);
    heard(c);
    /* STR 14 (+2), half the level +2, the scimitar +2. */
    CHECK(said("] +6 ="), "the scimitar's +2 to hit");

    hero(0, "blade", FLOOR, CX, CY);
    put_on(c, "Staff of Withering");
    rules_force_d20(19);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("strikes with magic") && c->slot_hand_r.current_charges == 2,
          "withering: %d charges", c->slot_hand_r.current_charges);
    c->slot_hand_r.current_charges = 0;
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(!said("strikes with magic"), "withering without charges");

    hero(0, "blade", FLOOR, CX, CY);
    put_on(c, "Shortsword of Life-Stealing");
    rules_force_d20(20);
    perform_attack(c, m, &g_game);
    rules_force_d20(0);
    CHECK(effect_value(c, EVENT_ON_TEMP_HP) >= 3, "life stealing: %d temporary HP",
          effect_value(c, EVENT_ON_TEMP_HP));

    hero(0, "blade", FLOOR, CX, CY);
    m->effects[m->effect_count++] =
        (ActiveEffect){"Invisible", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 50, false};
    rules_force_d20(15);
    perform_attack(c, m, &g_game);
    heard(c);
    CHECK(said("[DISADVANTAGE]"), "an invisible monster");
    put_on(c, "Robe of Eyes");
    perform_attack(c, m, &g_game);
    heard(c);
    rules_force_d20(0);
    CHECK(!said("[DISADVANTAGE]"), "the robe of eyes does not see the invisible");
    return 0;
}

/* Hits of n attacks of the monster, the dice free. */
static int hits_taken(NPC *m, Client *c, int n) {
    int hits = 0;
    for (int k = 0; k < n; k++) {
        c->hp = 1000;
        perform_attack_npc(m, c, &g_game);
        hits += c->hp < 1000;
    }
    c->hp = 60;
    return hits;
}

/* What items do to the round and to the world: regeneration, no hunger,
 * darkvision, the brooch against magic missiles, displacement, a fall
 * that does not hurt. */
int test_magic_items_world(void) {
    hx_reset_world();
    Client *c = hero(0, "keeper", FLOOR, CX, CY);
    CHECK(c != NULL, "no player");
    put_on(c, "Ring of Regeneration");
    c->hp = 30;
    g_game.total_turns = 100;
    player_items_round(&g_game, c);
    CHECK(c->hp > 30 && c->hp <= 36, "regeneration: HP %d", c->hp);
    g_game.total_turns = 101;
    int hp = c->hp;
    player_items_round(&g_game, c);
    CHECK(c->hp == hp, "regenerated out of time");

    give(c, item_named("Ioun Stone of Sustenance"), 1);
    c->hunger_level = 7;
    update_world(g_game.clients, &g_game);
    CHECK(c->hunger_level == 7, "sustenance: hunger %d", c->hunger_level);
    CHECK(get_vision_radius(c) == 4, "sight in the dark: %d", get_vision_radius(c));
    put_on(c, "Goggles of Night");
    CHECK(get_vision_radius(c) == 12, "goggles of night: %d", get_vision_radius(c));

    hero(0, "keeper", FLOOR, CX, CY);
    NPC *mage = monster(10, NULL, FLOOR, CX + 2, CY);
    mage->spell_slots[1] = 1;
    ai_mage_behavior(&g_game, mage, g_game.clients, MAX_CLIENTS, true);
    CHECK(c->hp < 60, "the mage's missiles missed");
    c->hp = 60;
    put_on(c, "Brooch of Shielding");
    mage->spell_slots[1] = 1;
    ai_mage_behavior(&g_game, mage, g_game.clients, MAX_CLIENTS, true);
    heard(c);
    CHECK(c->hp == 60 && said("stops the Magic Missile"), "the brooch: HP %d", c->hp);

    /* Armour enough for the monster to hit about half the time, then the
     * cloak: disadvantage squares the chance. */
    hero(0, "keeper", FLOOR, CX, CY);
    NPC *m = monster(11, NULL, FLOOR, CX + 1, CY);
    c->effects[c->effect_count++] =
        (ActiveEffect){"Magic: armour", EVENT_ON_DEFEND, MOD_ADDITIVE, 0, 100000, false};
    while (hits_taken(m, c, 200) > 120 && c->effects[0].value < 40) {
        c->effects[0].value++;
    }
    int plain = hits_taken(m, c, 400);
    put_on(c, "Cloak of Displacement");
    int displaced = hits_taken(m, c, 400);
    CHECK(plain > 40 && displaced + 20 < plain, "displacement: %d hits, %d without", displaced,
          plain);

    hero(0, "keeper", FLOOR, CX, CY);
    put_on(c, "Ring of Feather Falling");
    Floor *fl = &g_game.world->floors[FLOOR];
    Trap *t = &fl->traps[fl->trap_count++];
    memset(t, 0, sizeof(*t));
    t->type = TRAP_PIT;
    t->x = CX;
    t->y = CY;
    t->active = true;
    t->save_dc = 40;
    t->damage_dice = 4;
    t->damage_sides = 6;
    check_traps(c, &g_game);
    heard(c);
    CHECK(c->hp == 60 && said("float down"), "feather fall: HP %d", c->hp);
    return 0;
}
