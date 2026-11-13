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
 * cmd_equip.c - Equipment: wear, remove, belt, unbelt.
 */

#include "inventory.h"
#include "strnum.h"
#include "server_internal.h"
#include "player.h"
#include <ctype.h>

/*The equipment slot an item goes to: a free hand, arm or ring slot when
 * there are several (else the second hand or arm, the first ring); NULL
 * for an item that cannot be worn.*/
static ItemInstance *wear_slot(Client *c, ItemCategory category) {
    switch (category) {
    case ITEM_WEAPON:
    case ITEM_SHIELD:
        return c->slot_hand_r.template_idx == -1 ? &c->slot_hand_r : &c->slot_hand_l;
    case ITEM_HEAD:
        return &c->slot_head;
    case ITEM_NECK:
        return &c->slot_neck;
    case ITEM_ARMOR:
        return &c->slot_body;
    case ITEM_BACK:
        return &c->slot_back;
    case ITEM_HANDS:
        return &c->slot_hands;
    case ITEM_FEET:
        return &c->slot_feet;
    case ITEM_BRACELET:
        return c->slot_arm_r.template_idx == -1 ? &c->slot_arm_r : &c->slot_arm_l;
    case ITEM_RING:
        for (int r = 0; r < 10; r++) {
            if (c->slot_rings[r].template_idx == -1) {
                return &c->slot_rings[r];
            }
        }
        return &c->slot_rings[0];
    default:
        return NULL;
    }
}

/*Wearing an unidentified item reveals it; a cursed one is then stuck.*/
static void wear_announce(Client *c, ItemInstance *worn, const ItemTemplate *it) {
    if (worn->is_identified) {
        send_text_to_client(c->sock, "[SYSTEM] You have equipped %s.", it->name);
        return;
    }
    worn->is_identified = true;
    char id_buf[128];
    get_full_item_name(worn, id_buf, sizeof(id_buf));
    send_text_to_client(c->sock, "[MAGIC] By wearing the object you identify it: it's %s!", id_buf);
    if (worn->blessing == BLESS_CURSED) {
        send_text_to_client(c->sock, "[CURSE] A dark aura envelops your hand! "
                                     "The item is CURSED and you can't take it away!");
    }
}

/*wear <name|n>: equips an item of the backpack or the belt; the item it
 * replaces goes to the backpack. A cursed item cannot be replaced.*/
bool cmd_wear(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = strchr(cmd, ' ');
    if (!input) {
        return true;
    }
    input++;
    bool in_belt = false;
    int si = find_carried(c, input, &in_belt);
    if (si == -1) {
        send_text_to_client(c->sock, "[ERROR] Object not found (use name or backpack number).");
        return true;
    }
    ItemInstance worn = in_belt ? c->belt[si] : c->backpack[si];
    const ItemTemplate *it = &item_database[worn.template_idx];
    ItemInstance *slot = wear_slot(c, it->category);
    if (!slot) {
        send_text_to_client(c->sock, "[ERROR] You cannot wear this item.");
        return true;
    }
    ItemInstance prev = *slot;
    if (prev.template_idx != -1 && prev.blessing == BLESS_CURSED) {
        send_text_to_client(c->sock,
                            "[CURSE] The cursed %s will not let go: you cannot replace it.",
                            item_database[prev.template_idx].name);
        return true;
    }
    /*From the backpack the swap always fits; from the belt the replaced
     * item needs a free backpack slot.*/
    if (prev.template_idx != -1 && in_belt && c->backpack_count >= MAX_BACKPACK) {
        send_text_to_client(
            c->sock, "[ERROR] Your backpack is full: free a slot for the %s you would remove.",
            item_database[prev.template_idx].name);
        return true;
    }
    *slot = worn;
    if (in_belt) {
        c->belt[si].template_idx = -1;
    } else {
        c->backpack[si] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
    }
    if (prev.template_idx != -1) {
        c->backpack[c->backpack_count++] = prev;
    }
    wear_announce(c, slot, it);
    send_detailed_state(game, c);
    return true;
}

//===== MECHANICAL 1: REMOVE (remove equipped item) =====
/*"t <dir>" is tunnel, so this command has no one-letter alias.*/
bool cmd_remove(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = strchr(cmd, ' ');
    if (input) {
        input++;
    } else {
        return true;
    }
    //Slots map by name
    ItemInstance *sls[] = {&c->slot_head,   &c->slot_neck,   &c->slot_body,  &c->slot_back,
                           &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
                           &c->slot_arm_l,  &c->slot_feet};
    const char *snames[] = {"head",   "neck",  "body",  "back",  "hand_r",
                            "hand_l", "hands", "arm_r", "arm_l", "feet"};
    ItemInstance *target_slot = NULL;
    int scount = 10;
    //Search by slot name or item name
    for (int si2 = 0; si2 < scount; si2++) {
        if (sls[si2]->template_idx == -1) {
            continue;
        }
        const char *iname = item_database[sls[si2]->template_idx].name;
        bool slot_match = (strcasecmp(input, snames[si2]) == 0);
        if (!slot_match) {
            if ((si2 == 7 && (strcasecmp(input, "right_arm") == 0 ||
                              strcasecmp(input, "arm_r") == 0 || strcasecmp(input, "r.a") == 0)) ||
                (si2 == 8 && (strcasecmp(input, "left_arm") == 0 ||
                              strcasecmp(input, "arm_l") == 0 || strcasecmp(input, "l.a") == 0)) ||
                ((si2 == 7 || si2 == 8) &&
                 (strcasecmp(input, "arm") == 0 || strcasecmp(input, "arms") == 0))) {
                slot_match = true;
            }
        }
        if (strcasestr(iname, input) != NULL || slot_match ||
            (sls[si2]->is_artifact && strcasestr(sls[si2]->artifact_name, input) != NULL)) {
            target_slot = sls[si2];
            break;
        }
    }
    //Also check the rings
    if (!target_slot) {
        for (int ri = 0; ri < 10; ri++) {
            if (c->slot_rings[ri].template_idx == -1) {
                continue;
            }
            const char *iname = item_database[c->slot_rings[ri].template_idx].name;
            if (strcasestr(iname, input) != NULL) {
                target_slot = &c->slot_rings[ri];
                break;
            }
        }
    }
    //Also check your seat belt
    if (!target_slot) {
        for (int bi = 0; bi < MAX_BELT; bi++) {
            if (c->belt[bi].template_idx == -1) {
                continue;
            }
            const char *iname = item_database[c->belt[bi].template_idx].name;
            if (strcasestr(iname, input) != NULL) {
                target_slot = &c->belt[bi];
                break;
            }
        }
    }
    if (!target_slot) {
        send_text_to_client(c->sock, "[ERROR] Item not found among those equipped."
                                     "Use 'stats' to see what you're wearing.");
        return true;
    }
    //===== CURSE BLOCK (Heart of Mechanics 1) =====
    if (target_slot->blessing == BLESS_CURSED && target_slot->is_identified) {
        char curse_buf[128];
        get_full_item_name(target_slot, curse_buf, sizeof(curse_buf));
        send_text_to_client(c->sock,
                            "[CURSE] The object '%s' is surrounded by an evil aura!"
                            "You can't take it off!",
                            curse_buf);
        send_text_to_client(c->sock, "[TIP] Find a cleric and use 'uncurse' (200gp)"
                                     "or use a Scroll of Curse Removal.");
        return true;
    }
    //Normal removal: put in backpack
    if (c->backpack_count >= MAX_BACKPACK) {
        send_text_to_client(c->sock, "[ERROR] Backpack full! Empty your backpack first.");
        return true;
    }
    char rem_buf[128];
    get_full_item_name(target_slot, rem_buf, sizeof(rem_buf));
    c->backpack[c->backpack_count++] = *target_slot;
    target_slot->template_idx = -1;
    send_text_to_client(c->sock, "[SYSTEM] You have removed %s and moved it to the backpack.",
                        rem_buf);
    send_detailed_state(game, c);
    return true;
}

bool cmd_belt(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = cmd + 5;
    int bi = -1;
    if (isdigit(input[0])) {
        int requested_idx = list_index(input);
        if (requested_idx >= 0 && requested_idx < c->backpack_count) {
            bi = requested_idx;
        }
    } else {
        for (int i = 0; i < c->backpack_count; i++) {
            if (strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
                bi = i;
                break;
            }
        }
    }

    if (bi != -1) {
        for (int i = 0; i < MAX_BELT; i++) {
            if (c->belt[i].template_idx == -1) {
                c->belt[i] = c->backpack[bi];
                c->backpack[bi] = c->backpack[c->backpack_count - 1];
                c->backpack_count--;
                send_text_to_client(c->sock, "[SYSTEM] %s moved to belt.",
                                    item_database[c->belt[i].template_idx].name);
                send_detailed_state(game, c); //Update client view radius
                return true;
            }
        }
        send_text_to_client(c->sock, "[ERROR] Belt full!");
    } else {
        send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
    }
    return true;
}

bool cmd_unbelt(Client *c, const char *cmd, Game *game) {
    (void)game;
    const char *input = strchr(cmd, ' ');
    if (input) {
        input++;
    }
    int slot_idx = -1;
    if (input && isdigit(input[0])) {
        int requested_idx = list_index(input);
        if (requested_idx >= 0 && requested_idx < MAX_BELT &&
            c->belt[requested_idx].template_idx != -1) {
            slot_idx = requested_idx;
        }
    } else if (input) {
        for (int i = 0; i < MAX_BELT; i++) {
            if (c->belt[i].template_idx != -1 &&
                strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
                slot_idx = i;
                break;
            }
        }
    } else {
        /*If unbelt without arguments, move the first belted item into the backpack*/
        for (int i = 0; i < MAX_BELT; i++) {
            if (c->belt[i].template_idx != -1) {
                slot_idx = i;
                break;
            }
        }
    }

    if (slot_idx != -1) {
        if (c->backpack_count >= MAX_BACKPACK) {
            send_text_to_client(c->sock, "[ERROR] Backpack full!");
            return true;
        }
        ItemInstance item = c->belt[slot_idx];
        c->backpack[c->backpack_count++] = item;
        c->belt[slot_idx].template_idx = -1;
        c->belt[slot_idx].stack_count = 0;
        send_text_to_client(c->sock, "[SYSTEM] %s moved from belt to backpack.",
                            item_database[item.template_idx].name);
        send_detailed_state(game, c);
    } else {
        send_text_to_client(
            c->sock, "[ERROR] No items found in belt (use 'unbelt <n>' or 'unbelt <name>').");
    }
    return true;
}

// lore <name> — inspects an item in the backpack or reads its content if it is a book
