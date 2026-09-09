#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float visionRadius;
    float playerX;
    float playerZ;
    float time;
} pc;

void main() {
    float dx = fragWorldPos.x - pc.playerX;
    float dz = fragWorldPos.z - pc.playerZ;
    float dist = sqrt(dx*dx + dz*dz);
    float vr = pc.visionRadius;

    if (vr > 9000.0) {
        outColor = fragColor;
        return;
    }

    float inner = vr * 0.7;
    float fog = 1.0 - smoothstep(inner, vr, dist);

    float vignette = 1.0 - (dist / (vr * 1.2)) * 0.15;
    vignette = clamp(vignette, 0.5, 1.0);

    vec3 L = normalize(vec3(pc.playerX, 2.0, pc.playerZ) - fragWorldPos);
    float diff = max(dot(normalize(fragNormal), L), 0.2);

    vec3 finalColor = fragColor.rgb * diff * fog * vignette;

    if (dist > vr) {
        discard;
    }

    outColor = vec4(finalColor, fragColor.a);
}
