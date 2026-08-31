#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float visionRadius;
} pc;

void main() {
    /* Distanza del frammento dal centro del mondo (giocatore è a 0,0) */
    float dist = length(fragWorldPos.xz);
    float vr = pc.visionRadius;

    /* Se siamo oltre il raggio di visione molto grande, non usiamo fog/vignette (utile per HUD) */
    if (vr > 9000.0) {
        outColor = fragColor;
        return;
    }

    /* Fog of War morbida: sfumatura smooth ai bordi della visione */
    float inner = vr * 0.7;
    float fog = 1.0 - smoothstep(inner, vr, dist);

    /* Vignettatura sottile: oscura leggermente i bordi lontani */
    float vignette = 1.0 - (dist / (vr * 1.2)) * 0.15;
    vignette = clamp(vignette, 0.5, 1.0);

    vec3 L = normalize(vec3(0.0, 2.0, 0.0) - fragWorldPos);
    float diff = max(dot(normalize(fragNormal), L), 0.2);

    vec3 finalColor = fragColor.rgb * diff * fog * vignette;

    /* Oltre il raggio visivo: nero totale */
    if (dist > vr) {
        discard;
    }

    outColor = vec4(finalColor, fragColor.a);
}
