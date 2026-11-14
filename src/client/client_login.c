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
 * client_login.c - The questions asked on the terminal before the client
 * connects (client_login.h).
 */

#include "client_login.h"

#include "alignment.h"
#include "classes.h"
#include "net.h"
#include "rules.h"
#include "species.h"
#include "strnum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*Prompt for a secret without echoing it when stdin is a terminal. The
 * first word of the line is kept (passwords contain no spaces).*/
static void read_secret(const char *prompt, char *out, size_t out_len) {
    char line[128];
    struct termios saved, quiet;
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    printf("%s", prompt);
    fflush(stdout);
    if (tty) {
        quiet = saved;
        quiet.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    out[0] = '\0';
    if (fgets(line, sizeof(line), stdin)) {
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%%zus", out_len - 1);
        if (sscanf(line, fmt, out) != 1) {
            out[0] = '\0';
        }
    }
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        printf("\n");
    }
    memset(line, 0, sizeof(line));
    if (strlen(out) >= PROTO_SECRET_LEN) {
        printf("Warning: only the first %d characters are sent to the server.\n",
               PROTO_SECRET_LEN - 1);
    }
}

/*A number typed on a line; fallback when the line is empty, not a number
 * or outside 0..count-1.*/
static int ask_index(const char *prompt, int count, int fallback) {
    char buf[128];
    int v = fallback;
    printf("%s", prompt);
    if (fgets(buf, sizeof(buf), stdin)) {
        parse_int(buf, &v, NULL);
    }
    return v >= 0 && v < count ? v : fallback;
}

static int ask_race(void) {
    printf("\nAvailable Races:\n");
    for (int i = 0; i < RACE_COUNT; i++) {
        printf("  %2d = %-16s : %s\n", i, RACES[i].name, RACES[i].description);
    }
    return ask_index("Race ID: ", RACE_COUNT, 0);
}

/*-1 when the race has no subraces (nothing is asked) or none is chosen.*/
static int ask_subrace(int race_id) {
    bool any = false;
    printf("\nSubraces available for %s:\n", RACES[race_id].name);
    for (int i = 0; i < SUBRACE_COUNT; i++) {
        if (SUBRACES[i].parent_race == (RaceType)race_id) {
            printf("  %2d = %-16s : %s\n", i, SUBRACES[i].name, SUBRACES[i].description);
            any = true;
        }
    }
    if (!any) {
        return -1;
    }
    int id = ask_index("Choose Subrace (ID) or -1 for none: ", SUBRACE_COUNT, -1);
    return id >= 0 && SUBRACES[id].parent_race == (RaceType)race_id ? id : -1;
}

static int ask_class(void) {
    printf("\nAvailable Classes:\n");
    for (int i = 0; i < CLASS_COUNT; i++) {
        printf("  %2d = %-12s : %s\n", i, CLASSES[i].name, CLASSES[i].description);
    }
    return ask_index("Class ID: ", CLASS_COUNT, 0);
}

static bool ask_female(void) {
    char buf[128];
    printf("Sex (M=Male, F=Female): ");
    return fgets(buf, sizeof(buf), stdin) && (buf[0] == 'f' || buf[0] == 'F');
}

static int ask_alignment(void) {
    printf("Alignment:\n");
    for (int i = 0; i < ALIGN_COUNT; i++) {
        printf("  %d=%-16s", i, ALIGNMENTS[i].name);
        if ((i + 1) % 3 == 0) {
            printf("\n");
        }
    }
    return ask_index("Choice: ", ALIGN_COUNT, 0);
}

/*The sheet of the new character. Age, height, weight and social status
 * are flavour rolled here; the ability scores are rolled by the server (a
 * client could send anything) and arrive as a [CHARACTER] message.*/
static void print_sheet(const LoginAnswers *who, bool female) {
    char race[64];
    srand((unsigned int)time(NULL));
    int age = rules_roll_dice(3, 6) + 15;
    int height = rules_roll_dice(4, 10) + 140;
    int weight = rules_roll_dice(4, 10) + 50;
    int social = rules_roll_dice(1, 100);
    if (who->subrace_id != -1) {
        snprintf(race, sizeof(race), "%s (%s)", RACES[who->race_id].name,
                 SUBRACES[who->subrace_id].name);
    } else {
        copy_str(race, RACES[who->race_id].name, sizeof(race));
    }
    printf("\n========================================================================\n");
    printf(" Name        : %-22s Age          : %5d\n", who->username, age);
    printf(" Race        : %-22s Height (cm)  : %5d\n", race, height);
    printf(" Sex         : %-22s Weight (kg)  : %5d\n", female ? "Female" : "Male", weight);
    printf(" Class       : %-22s Social Status: %5d\n", CLASSES[who->class_id].name, social);
    printf(" Alignment   : %s\n", ALIGNMENTS[who->alignment].name);
    printf(" Traits      : %s%s%s\n", RACES[who->race_id].traits, who->subrace_id != -1 ? ", " : "",
           who->subrace_id != -1 ? SUBRACES[who->subrace_id].traits : "");
    printf(" STR/DEX/CON/INT/WIS/CHA are rolled by the server: see the\n");
    printf(" [CHARACTER] message after login.\n");
    printf("========================================================================\n\n");
}

void login_prompt(LoginAnswers *out, char *host, size_t host_cap, bool ask_host) {
    char buf[256];
    memset(out, 0, sizeof(*out));
    out->subrace_id = -1;
    printf("\n--- Dragon GL Client ---\n");
    if (!ask_host) {
        printf("Server host: %s (from command line)\n", host);
    } else {
        printf("Server host [%s]: ", host);
        if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
            buf[strcspn(buf, " \t\r\n")] = '\0';
            if (buf[0]) {
                copy_str(host, buf, host_cap);
            }
        }
    }
    read_secret("Server Password: ", out->server_pass, sizeof(out->server_pass));
    printf("Player Name: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%31s", out->username);
    }
    read_secret("Personal Password: ", out->password, sizeof(out->password));
    if (ask_index("New character? (1=Yes, 0=No):", 2, 0) == 0) {
        return;
    }
    out->race_id = ask_race();
    out->subrace_id = ask_subrace(out->race_id);
    out->class_id = ask_class();
    bool female = ask_female();
    out->alignment = ask_alignment();
    print_sheet(out, female);
}
