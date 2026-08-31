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

#ifndef RENDER_GL_H
#define RENDER_GL_H

#ifdef __cplusplus
extern "C" {
#endif

void render_gl_start(void);
void render_gl_hud(int width, int height);
void render_gl_hud_window(int width, int height);
void gl_spawn_vfx(int type, float sx, float sy, float tx, float ty, float r, float g, float b);

#ifdef __cplusplus
}
#endif

#endif // RENDER_GL_H
