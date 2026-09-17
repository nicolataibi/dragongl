#version 450
/*
 * DRAGON GL - GPD scene vertex shader (GPU-driven path).
 * (GPL-3.0-or-later)
 *
 * Reads the compute-generated vertices from the storage buffer (there
 * is NO fixed-function vertex input on this pipeline). Outputs and
 * push constants are identical to the legacy shader.vert, so the
 * fragment shader (shader.frag) is shared unchanged.
 */

struct GpdVertex {
    vec3 pos;
    vec4 color;
    vec3 normal;
};

layout(binding = 0) buffer SceneVerts { GpdVertex verts[]; } gpd_scene_verts;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float visionRadius;
    float playerX;
    float playerZ;
    float time;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec3 fragNormal;

void main() {
    GpdVertex gv = gpd_scene_verts.verts[gl_VertexIndex];
    vec3 pos = gv.pos;

    // Animate water (approx color: 0.1, 0.4, 0.8) and lava (1.0, 0.3, 0.0)
    // — identical to the legacy vertex shader.
    bool is_water = abs(gv.color.r - 0.1) < 0.01 && abs(gv.color.g - 0.4) < 0.01 && abs(gv.color.b - 0.8) < 0.01;
    bool is_lava  = abs(gv.color.r - 1.0) < 0.01 && abs(gv.color.g - 0.3) < 0.01 && abs(gv.color.b - 0.0) < 0.01;

    if (is_water || is_lava) {
        pos.y += sin(pc.time * 2.0 + pos.x + pos.z) * 0.1;
    }

    gl_Position = pc.mvp * vec4(pos, 1.0);
    fragColor = gv.color;
    fragWorldPos = pos;
    fragNormal = gv.normal;
}
