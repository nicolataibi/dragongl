/*
 * DRAGON GL - GPU-driven mesh generation (GPD)
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
 *
 * Common definitions shared by all gpd_*.comp shaders.
 * NOTE: included by the gpd_*.comp entry points — no #version here
 * (it must appear only in the entry file). NOTE 2: this glslang build
 * requires a block instance name on every buffer declaration, so all
 * blocks below carry one.
 *
 * The CPU uploads compact INSTANCE descriptors (see GpdInstance /
 * GpdOriented in src/client/render_vk.h — keep the layouts in sync);
 * the compute pass culls instances by vision radius and expands the
 * primitive (box / pyramid / rotated box) into the device-local vertex
 * SSBO, accumulating the total in an atomic counter. The fragment
 * shader still discards anything beyond visionRadius, so culling here
 * only needs to be a SUPERSET of what is visible.
 */

/* Vertices generated into gpd_verts (48 B stride == VKD_VERTEX_STRIDE).
 * Same semantic fields as the legacy VkVertex (pos, color, normal). */
struct GpdVertex {
    vec3 pos;
    vec4 color;
    vec3 normal;
};

/* Instance descriptor (48 B == GpdInstance).
 * a = (pos.xyz, type), b = (scale.xyz, pad), c = (color.xyz, pad). */
struct GpdInstance {
    vec4 a;
    vec4 b;
    vec4 c;
};

/* Rotated box descriptor (64 B == GpdOriented). o0/o1/o2 = mat3 columns. */
struct GpdOriented {
    vec4 a;
    vec4 b;
    vec4 c;
    float m00, m01, m02;
    float m10, m11, m12;
    float m20, m21, m22;
    float p0, p1, p2;
};

/* Per-frame GPU counters (16 B buffer; CPU side: GpdCounts in
 * src/client/render_vk.h — keep in sync). Each entry shader declares a
 * block wrapping it:
 *   layout(binding = N) buffer Counts { GpdCounts c; } gpd_counts;
 * The CPU zeroes the whole buffer at the start of every frame (after the
 * per-slot fence wait), so no shader ever resets a counter. */
struct GpdCounts {
    uint verts;   /* running total of generated vertices (expand -> final) */
    uint map_vis; /* visible map instances (cull_map -> expand_map) */
    uint dyn_vis; /* visible dynamic instances (cull_dyn -> expand_dyn) */
};

/* Push constants (24 B == GpdPush). cull = (radius, playerX, playerZ,
 * time). */
layout(push_constant) uniform GpdPC {
    vec4 cull;
    uint count;
    uint capacity;
} pc;

const int GPD_TYPE_BOX = 0;
const int GPD_TYPE_PYRAMID = 1;
const int GPD_TYPE_PARTICLE = 2; /* box, never culled */

/* Box expansion: identical face/corner order to the CPU push_box()
 * (top, bottom, front, back, left, right; two triangles per face).
 * Entry i belongs to face i/6, whose normal is GPD_BOX_FACE_N[i/6]. */
const ivec3 GPD_BOX_SIGNS[36] = {
    /* top (n = 0,1,0) */
    ivec3(-1, 1, -1), ivec3( 1, 1, -1), ivec3(-1, 1, 1),
    ivec3( 1, 1, -1), ivec3( 1, 1, 1),  ivec3(-1, 1, 1),
    /* bottom (n = 0,-1,0) */
    ivec3(-1, -1, -1), ivec3(-1, -1, 1),  ivec3( 1, -1, -1),
    ivec3( 1, -1, -1), ivec3(-1, -1, 1),  ivec3( 1, -1, 1),
    /* front (n = 0,0,1) */
    ivec3(-1, -1, 1),  ivec3( 1, -1, 1),  ivec3(-1, 1, 1),
    ivec3( 1, -1, 1),  ivec3( 1, 1, 1),   ivec3(-1, 1, 1),
    /* back (n = 0,0,-1) */
    ivec3(-1, -1, -1), ivec3(-1, 1, -1),  ivec3( 1, -1, -1),
    ivec3( 1, -1, -1), ivec3(-1, 1, -1),  ivec3( 1, 1, -1),
    /* left (n = -1,0,0) */
    ivec3(-1, -1, -1), ivec3(-1, -1, 1),  ivec3(-1, 1, -1),
    ivec3(-1, -1, 1),  ivec3(-1, 1, 1),   ivec3(-1, 1, -1),
    /* right (n = 1,0,0) */
    ivec3( 1, -1, -1), ivec3( 1, 1, -1),  ivec3( 1, -1, 1),
    ivec3( 1, -1, -1), ivec3( 1, 1, 1),   ivec3( 1, -1, 1)
};

const vec3 GPD_BOX_FACE_N[6] = {
    vec3(0.0,  1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0,  1.0),
    vec3(0.0,  0.0, -1.0), vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0)
};

/* Pyramid (18 vertices): identical order to the CPU push_pyramid().
 * The apex encodes as (0,1,0) * scale, which is exact (apex = center +
 * (0, hy, 0)). */
const ivec3 GPD_PYM_SIGNS[18] = {
    ivec3( 0, 1,  0), ivec3(-1, -1, 1),  ivec3( 1, -1, 1),
    ivec3( 0, 1,  0), ivec3( 1, -1, 1),  ivec3( 1, -1, -1),
    ivec3( 0, 1,  0), ivec3( 1, -1, -1), ivec3(-1, -1, -1),
    ivec3( 0, 1,  0), ivec3(-1, -1, -1), ivec3(-1, -1, 1),
    ivec3(-1, -1, -1), ivec3( 1, -1, -1), ivec3(-1, -1, 1),
    ivec3( 1, -1, -1),  ivec3( 1, -1, 1),  ivec3(-1, -1, 1)
};

/* Same (unnormalized, like the CPU) per-vertex normals. */
const vec3 GPD_PYM_N[18] = {
    vec3(0.0, 0.5, 1.0),  vec3(0.0, 0.5, 1.0),  vec3(0.0, 0.5, 1.0),
    vec3(1.0, 0.5, 0.0),  vec3(1.0, 0.5, 0.0),  vec3(1.0, 0.5, 0.0),
    vec3(0.0, 0.5, -1.0), vec3(0.0, 0.5, -1.0), vec3(0.0, 0.5, -1.0),
    vec3(-1.0, 0.5, 0.0), vec3(-1.0, 0.5, 0.0), vec3(-1.0, 0.5, 0.0),
    vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0)
};
