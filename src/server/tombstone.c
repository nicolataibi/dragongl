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
 * tombstone.c — Tombstone System (DragonGL)
 *
 * When a player dies, a Tombstone is created in them
 * exact position (plane + coordinates). The tombstone:
 * - Appears on the map with the 'T' symbol
 * - Contains all of his inventory, equipment and gold
 * - Can be inspected with 'look' or 'list'
 * - Can ONLY be recovered by the owner player with 'pickup'
 * - It is saved to disk in saves/tombstone_<owner>.dat
 * - It is loaded when the server starts*/

#include "server_internal.h"
#include "server.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>

/*Global tombstone array*/
Tombstone g_tombstones[MAX_TOMBSTONES];

/*---------------------------------------------------------------
 * tombstone_is_expired
* Checks whether a tombstone has expired.
 * ---------------------------------------------------------------*/
bool tombstone_is_expired(const Tombstone *t) {
    if (!t || !t->active) {
        return true;
    }
    
    time_t now = time(NULL);
    double hours_diff = difftime(now, t->death_time) / 3600.0;
    
    return hours_diff > TOMBSTONE_EXPIRE_HOURS;
}

/*---------------------------------------------------------------
 * tombstone_save
 * Save a single tombstone to disk.
 * File: saves/tombstone_<owner>.dat
 * ---------------------------------------------------------------*/
void tombstone_save(const Tombstone *t) {
    if (!t || !t->active) {
        return;
    }
    
    //Creating the directory securely
    struct stat st = {0};
    if (stat("saves", &st) == -1) {
        if (mkdir("saves", 0755) != 0) {
            server_log("TOMB", "ERROR: failed to create saves/ directory (%s)", strerror(errno));
            return;
        }
    }
    
    char path[256];
    snprintf(path, sizeof(path), "saves/tombstone_%s_%ld_%d_%d.dat", t->owner, (long)t->death_time, t->x, t->y);
    FILE *f = fopen(path, "wb");
    if (!f) {
        server_log("TOMB", "ERROR: failed to open %s", path);
        return;
    }
    
    size_t written = fwrite(t, sizeof(Tombstone), 1, f);
    fclose(f);
    
    if (written != 1) {
        server_log("TOMB", "ERROR: failed to write %s's tombstone entirely", t->owner);
        return;
    }
    
    server_log("TOMB", "Tombstone of %s saved to %s", t->owner, path);
}

/*---------------------------------------------------------------
 * tombstone_load
* Load a single tombstone from disk.
 * Returns true if loading succeeded, false otherwise.
 * ---------------------------------------------------------------*/
bool tombstone_load(const char *owner, Tombstone *out_tombstone) {
    if (!owner || !out_tombstone) {
        return false;
    }
    
    char path[128];
    snprintf(path, sizeof(path), "saves/tombstone_%s.dat", owner);
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    
    size_t read_bytes = fread(out_tombstone, 1, sizeof(Tombstone), f);
    fclose(f);
    
    if (read_bytes != sizeof(Tombstone)) {
        server_log("TOMB", "Corrupted tombstone file: %s", path);
        return false;
    }
    
    /* Check if the tombstone has expired */
    if (tombstone_is_expired(out_tombstone)) {
        remove(path);
        server_log("TOMB", "Expired tombstone deleted: %s", path);
        return false;
    }
    
    return true;
}



/*---------------------------------------------------------------
 * tombstone_delete_file
* Removes a tombstone's disk file (after recovery).
 * ---------------------------------------------------------------*/
static void tombstone_delete_file(const Tombstone *t) {
    char path[256];
    snprintf(path, sizeof(path), "saves/tombstone_%s_%ld_%d_%d.dat", t->owner, (long)t->death_time, t->x, t->y);
    remove(path);
    /*Backwards compatibility for old saves*/
    snprintf(path, sizeof(path), "saves/tombstone_%s.dat", t->owner);
    remove(path);
    server_log("TOMB", "Tombstone file of %s removed", t->owner);
}

/*---------------------------------------------------------------
 * tombstone_load_all
 * Load all tombstones from disk on startup.
 * Called by main_server.c after world initialization.
 * ---------------------------------------------------------------*/
void tombstone_load_all(void) {
    memset(g_tombstones, 0, sizeof(g_tombstones));
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        g_tombstones[i].active = false;
    }

    /*Reads all saved tombstones from the saves/ directory*/
    DIR *dir = opendir("saves");
    if (!dir) {
        return;
    }
    struct dirent *entry;
    int loaded = 0;
    while ((entry = readdir(dir)) != NULL && loaded < MAX_TOMBSTONES) {
        if (strncmp(entry->d_name, "tombstone_", 10) != 0) {
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "saves/%s", entry->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        Tombstone tmp;
        memset(&tmp, 0, sizeof(Tombstone));
        size_t read_bytes = fread(&tmp, 1, sizeof(Tombstone), f);
        fclose(f);
        if (read_bytes != sizeof(Tombstone)) {
            server_log("TOMB", "Corrupted tombstone file: %s (skip)", path);
            continue;
        }
        
        /* Check if the tombstone has expired */
        if (tombstone_is_expired(&tmp)) {
            remove(path);
            server_log("TOMB", "Expired tombstone deleted: %s", path);
            continue;
        }
        
        /*Validation of tombstone data*/
        if (tmp.x < 0 || tmp.y < 0 || tmp.floor_id < 0) {
            server_log("TOMB", "Invalid tombstone file (negative coordinates): %s (skip)", path);
            continue;
        }
        
        if (strlen(tmp.owner) == 0) {
            server_log("TOMB", "Invalid tombstone file (empty owner name): %s (skip)", path);
            continue;
        }
        
        /*Find a free slot*/
        for (int i = 0; i < MAX_TOMBSTONES; i++) {
            if (!g_tombstones[i].active) {
                g_tombstones[i] = tmp;
                g_tombstones[i].active = true;
                loaded++;
                server_log("TOMB", "Loaded tombstone of %s (floor %d, %d,%d)",
                           tmp.owner, tmp.floor_id, tmp.x, tmp.y);
                break;
            }
        }
    }
    closedir(dir);
    server_log("TOMB", "%d tombstones loaded", loaded);
}

/*---------------------------------------------------------------
 * tombstone_create
 * Creates a new tombstone for a newly deceased player.
 * Called by save_bones() before emptying the inventory.
 * ---------------------------------------------------------------*/
void tombstone_create(Client *c) {
    if (!c) {
        return;
    }
    
    /*Do not create tombstones on floor 0 (city)*/
    if (c->floor_id <= 0) {
        server_log("TOMB", "WARNING: attempt to create tombstone in city for %s, ignored", c->username);
        return;
    }

    /*Look for a free slot*/
    int slot = -1;
    if (slot == -1) {
        for (int i = 0; i < MAX_TOMBSTONES; i++) {
            if (!g_tombstones[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot == -1) {
        server_log("TOMB", "WARNING: no free tombstone slots, reusing the oldest");
        /*Reuse the older one*/
        time_t oldest = g_tombstones[0].death_time;
        slot = 0;
        for (int i = 1; i < MAX_TOMBSTONES; i++) {
            if (g_tombstones[i].death_time < oldest) {
                oldest = g_tombstones[i].death_time;
                slot = i;
            }
        }
        tombstone_delete_file(&g_tombstones[slot]);
    }

    Tombstone *t = &g_tombstones[slot];
    memset(t, 0, sizeof(Tombstone));
    t->active       = true;
    t->x            = c->x;
    t->y            = c->y;
    t->floor_id     = c->floor_id;
    t->death_time   = time(NULL);
    strncpy(t->owner, c->username, sizeof(t->owner) - 1);

    t->gold         = c->gold;
    t->backpack_count = c->backpack_count;
    memcpy(t->backpack, c->backpack, sizeof(c->backpack));
    memcpy(t->belt,    c->belt,    sizeof(c->belt));

    t->slot_head    = c->slot_head;
    t->slot_neck    = c->slot_neck;
    t->slot_body    = c->slot_body;
    t->slot_back    = c->slot_back;
    t->slot_hand_r  = c->slot_hand_r;
    t->slot_hand_l  = c->slot_hand_l;
    t->slot_hands   = c->slot_hands;
    t->slot_arm_r   = c->slot_arm_r;
    t->slot_arm_l   = c->slot_arm_l;
    t->slot_feet    = c->slot_feet;
    memcpy(t->slot_rings, c->slot_rings, sizeof(c->slot_rings));

    tombstone_save(t);

    server_log("TOMB", "Tombstone created for %s on floor %d (%d,%d)",
               c->username, c->floor_id, c->x, c->y);
}

/*---------------------------------------------------------------
 * tombstone_list
 * Show the player the contents of the tombstone at his location
 * (same or adjacent cell <= 1). Works like a merchant.
 * ---------------------------------------------------------------*/
void tombstone_list(Client *c) {
    if (!c) {
        return;
    }
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        Tombstone *t = &g_tombstones[i];
        if (!t->active) {
            continue;
        }
        if (t->floor_id != c->floor_id) {
            continue;
        }
        int dist = abs(t->x - c->x) + abs(t->y - c->y);
        if (dist > 1) {
            continue;
        }

        /*Format the date of death*/
        char timestr[32];
        struct tm *tm_info = localtime(&t->death_time);
        strftime(timestr, sizeof(timestr), "%d/%m/%Y %H:%M", tm_info);

        send_text_to_client(c->sock,
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        send_text_to_client(c->sock,
            "⚰  TOMBSTONE — %s", t->owner);
        send_text_to_client(c->sock,
            "Floor %d | Position (%d,%d) | Died on: %s",
            t->floor_id, t->x, t->y, timestr);
        send_text_to_client(c->sock,
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

        if (t->gold > 0) {
            send_text_to_client(c->sock, "[GOLD] %lu gp", (unsigned long)t->gold);
        }

        /*Backpack*/
        bool has_items = false;
        for (int j = 0; j < TOMBSTONE_BACKPACK_SIZE; j++) {
            if (t->backpack[j].template_idx < 0) {
                continue;
            }
            if (!has_items) {
                send_text_to_client(c->sock, "--- BACKPACK ---");
                has_items = true;
            }
            char name[128];
            get_full_item_name(&t->backpack[j], name, sizeof(name));
            send_text_to_client(c->sock, "   [%2d] %s (x%d)",
                j + 1, name, t->backpack[j].stack_count > 0 ? t->backpack[j].stack_count : 1);
        }

        /* Equipment */
        const ItemInstance *eq[] = {
            &t->slot_head, &t->slot_neck, &t->slot_body,
            &t->slot_back, &t->slot_hand_r, &t->slot_hand_l,
            &t->slot_hands, &t->slot_arm_r, &t->slot_arm_l, &t->slot_feet
        };
        const char *eq_names[] = {
            "HEAD", "NECK", "BODY", "BACK",
            "R.HAND", "L.HAND", "HANDS", "R.ARM", "L.ARM", "FEET"
        };
        bool has_eq = false;
        for (int j = 0; j < 10; j++) {
            if (eq[j]->template_idx < 0) {
                continue;
            }
            if (!has_eq) {
                send_text_to_client(c->sock, "   --- EQUIPMENT ---");
                has_eq = true;
            }
            char name[128];
            get_full_item_name(eq[j], name, sizeof(name));
            send_text_to_client(c->sock, "   [%s] %s", eq_names[j], name);
        }
        for (int j = 0; j < TOMBSTONE_RINGS; j++) {
            if (t->slot_rings[j].template_idx < 0) {
                continue;
            }
            char name[128];
            get_full_item_name(&t->slot_rings[j], name, sizeof(name));
            send_text_to_client(c->sock, "   [RING %d] %s", j + 1, name);
        }
        /*Belt*/
        bool has_belt = false;
        for (int j = 0; j < 4; j++) {
            if (t->belt[j].template_idx < 0) {
                continue;
            }
            if (!has_belt) {
                send_text_to_client(c->sock, "--- BELT ---");
                has_belt = true;
            }
            char name[128];
            get_full_item_name(&t->belt[j], name, sizeof(name));
            send_text_to_client(c->sock, "   [SLOT %d] %s", j + 1, name);
        }

        send_text_to_client(c->sock,
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        if (strcmp(c->username, t->owner) == 0) {
            send_text_to_client(c->sock,
                "Type 'pickup' to recover your items.");
        } else {
            send_text_to_client(c->sock,
                "[WARNING] Only %s can recover these items.", t->owner);
        }
        return;
    }
    send_text_to_client(c->sock, "[SYSTEM] No tombstones nearby.");
}

/*---------------------------------------------------------------
 * tombstone_pickup
 * Allows the owning player to recover all items
 * from the tombstone and place them in your inventory.
 * Returns true if recovery occurred, false otherwise.
 * ---------------------------------------------------------------*/
bool tombstone_pickup(Client *c) {
    if (!c) {
        return false;
    }
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
        Tombstone *t = &g_tombstones[i];
        if (!t->active) {
            continue;
        }
        if (t->floor_id != c->floor_id) {
            continue;
        }
        int dist = abs(t->x - c->x) + abs(t->y - c->y);
        if (dist > 1) {
            continue;
        }

        /*Found a nearby tombstone*/
        if (strcmp(c->username, t->owner) != 0) {
            send_text_to_client(c->sock,
                "[WARNING] This tombstone belongs to %s. You cannot take items from it.",
                t->owner);
            return false;
        }

        /*Recover gold*/
        c->gold += t->gold;

        /*Retrieve backpack (place in player's backpack)*/
        for (int j = 0; j < TOMBSTONE_BACKPACK_SIZE; j++) {
            if (t->backpack[j].template_idx < 0) {
                continue;
            }
            if (c->backpack_count >= MAX_BACKPACK) {
                send_text_to_client(c->sock,
                    "[WARNING] Backpack full! Some items cannot be recovered.");
                break;
            }
            c->backpack[c->backpack_count] = t->backpack[j];
            c->backpack_count++;
        }

        /*Recover belt (fill free slots)*/
        for (int j = 0; j < 4; j++) {
            if (t->belt[j].template_idx < 0) {
                continue;
            }
            for (int k = 0; k < MAX_BELT; k++) {
                if (c->belt[k].template_idx < 0) {
                    c->belt[k] = t->belt[j];
                    break;
                }
            }
        }

        /*Recovers equipment slots (only if empty in the character)*/
        if (c->slot_head.template_idx < 0)    c->slot_head    = t->slot_head;
        if (c->slot_neck.template_idx < 0)    c->slot_neck    = t->slot_neck;
        if (c->slot_body.template_idx < 0)    c->slot_body    = t->slot_body;
        if (c->slot_back.template_idx < 0)    c->slot_back    = t->slot_back;
        if (c->slot_hand_r.template_idx < 0)  c->slot_hand_r  = t->slot_hand_r;
        if (c->slot_hand_l.template_idx < 0)  c->slot_hand_l  = t->slot_hand_l;
        if (c->slot_hands.template_idx < 0)   c->slot_hands   = t->slot_hands;
        if (c->slot_arm_r.template_idx < 0)   c->slot_arm_r   = t->slot_arm_r;
        if (c->slot_arm_l.template_idx < 0)   c->slot_arm_l   = t->slot_arm_l;
        if (c->slot_feet.template_idx < 0)    c->slot_feet    = t->slot_feet;
        for (int j = 0; j < TOMBSTONE_RINGS; j++) {
            if (t->slot_rings[j].template_idx < 0) {
                continue;
            }
            for (int k = 0; k < 10; k++) {
                if (c->slot_rings[k].template_idx < 0) {
                    c->slot_rings[k] = t->slot_rings[j];
                    break;
                }
            }
        }

        /*Removes the tombstone from memory and disk*/
        t->active = false;
        tombstone_delete_file(t);

        /*Broadcast MSG_TOMBSTONE_REMOVE to all clients on the same plane*/

        int tomb_entity_id = -(i + 1);
        MsgHeader rm_hdr = {MSG_TOMBSTONE_REMOVE, sizeof(MsgTombstoneRemove)};
        MsgTombstoneRemove rm_msg;
        rm_msg.entity_id = tomb_entity_id;
        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (!g_clients[ci].active) {
                continue;
            }
            if (g_clients[ci].floor_id != t->floor_id) {
                continue;
            }
            net_send(g_clients[ci].sock, &rm_hdr, sizeof(rm_hdr));
            net_send(g_clients[ci].sock, &rm_msg, sizeof(rm_msg));
        }

        send_text_to_client(c->sock,
            "[SYSTEM] You have recovered your items from the tombstone. Gold recovered: %lu gp.",
            (unsigned long)t->gold);
        server_log("TOMB", "%s recovered their tombstone on floor %d (%d,%d)",
                   c->username, t->floor_id, t->x, t->y);
        return true;

    }
    send_text_to_client(c->sock,
        "[SYSTEM] No gravestone of yours nearby.");
    return false;
}
