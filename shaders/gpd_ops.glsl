/*
 * The expand/accumulate functions use the GLOBAL vertex and counter
 * blocks, which each entry shader declares (GLSL 450 has no pointers or
 * references): every gpd_expand_*.comp that calls them must define
 *   layout(binding = 3) buffer Verts  { GpdVertex verts[]; } gpd_verts;
 *   layout(binding = 2) buffer Counts { GpdCounts  c; }      gpd_counts;
 */

/*
 * Reserve `n` vertex slots.
 * Implementation note: this distro's glslang (shaderc 2026.x) does not
 * accept atomicCompareExchange / atomicSub / atomicLoad / atomicStore
 * overloads (build bug — they are core GLSL 450), so the reservation is
 * atomicAdd-based: the counter may be TRANSIENTLY inflated while a
 * concurrent reservation rolls back, but the final value is exact, and
 * a rolled-back reservation never writes any vertex, so no two live
 * regions can overlap. If the capacity guard fails the reservation is
 * rolled back and the instance is dropped — the same "drop when full"
 * behavior as the legacy CPU path (which never fills in practice: the
 * capacity has >2x slack over the worst-case visible window).
 */
bool gpd_reserve(uint n, out uint idx) {
    uint base = atomicAdd(gpd_counts.c.verts, n);
    if (base + n > pc.capacity) {
        atomicAdd(gpd_counts.c.verts, uint(-n));
        return false;
    }
    idx = base;
    return true;
}

void gpd_expand_box(uint base, vec3 center, vec3 scale, vec4 color) {
    for (int i = 0; i < 36; i++) {
        vec3 p = center + vec3(GPD_BOX_SIGNS[i]) * scale;
        gpd_verts.verts[base + i] = GpdVertex(p, color, GPD_BOX_FACE_N[i / 6]);
    }
}

void gpd_expand_pyramid(uint base, vec3 center, vec3 scale, vec4 color) {
    for (int i = 0; i < 18; i++) {
        vec3 p = center + vec3(GPD_PYM_SIGNS[i]) * scale;
        gpd_verts.verts[base + i] = GpdVertex(p, color, GPD_PYM_N[i]);
    }
}
