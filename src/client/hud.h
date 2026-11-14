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
 * hud.h - The HUD: the panels (character, minimap, equipment, trophies,
 * messages and command line, list of keys) and the labels drawn over the
 * 3D scene (names, combat numbers, compass).
 */

#ifndef HUD_H
#define HUD_H

#include "client_input.h"
#include "client_state.h"
#include "hud_draw.h"

/*The panels. Reads the character state: the caller holds g_state_mutex.
 * frame_ms: average frame time, shown when ui->show_fps.*/
void hud_panels(Hud *h, const ClientUi *ui, const ClientConfig *cfg, float frame_ms);
/*The labels over the scene, from the frame snapshot (no lock held).
 * (px, pz): the player's tile.*/
void hud_world_labels(Hud *h, const float mvp[16], const FrameSnapshot *snap, float px, float pz,
                      bool reduce_motion);

/*Rows of the message panel that are scrolled past the oldest message are
 * given back here, so that scrolling stops at the top.*/
int hud_log_scroll_limit(void);

#endif /* HUD_H */
