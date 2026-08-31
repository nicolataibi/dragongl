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

#ifndef ALIGNMENT_H
#define ALIGNMENT_H

typedef enum {
    ALIGN_LAWFUL_GOOD,
    ALIGN_NEUTRAL_GOOD,
    ALIGN_CHAOTIC_GOOD,
    ALIGN_LAWFUL_NEUTRAL,
    ALIGN_TRUE_NEUTRAL,
    ALIGN_CHAOTIC_NEUTRAL,
    ALIGN_LAWFUL_EVIL,
    ALIGN_NEUTRAL_EVIL,
    ALIGN_CHAOTIC_EVIL,
    ALIGN_COUNT
} AlignmentType;

typedef struct {
    const char* name;
} AlignmentTemplate;

static const AlignmentTemplate ALIGNMENTS[ALIGN_COUNT] = {
    [ALIGN_LAWFUL_GOOD]    = { "Lawful Good" },
    [ALIGN_NEUTRAL_GOOD]   = { "Neutral Good" },
    [ALIGN_CHAOTIC_GOOD]   = { "Chaotic Good" },
    [ALIGN_LAWFUL_NEUTRAL] = { "Lawful Neutral" },
    [ALIGN_TRUE_NEUTRAL]   = { "True Neutral" },
    [ALIGN_CHAOTIC_NEUTRAL]= { "Chaotic Neutral" },
    [ALIGN_LAWFUL_EVIL]    = { "Lawful Evil" },
    [ALIGN_NEUTRAL_EVIL]   = { "Neutral Evil" },
    [ALIGN_CHAOTIC_EVIL]   = { "Chaotic Evil" }
};

#endif // ALIGNMENT_H
