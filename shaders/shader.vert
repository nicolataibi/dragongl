#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;

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
    vec3 pos = inPosition;
    
    // Animate water (approx color: 0.1, 0.4, 0.8) and lava (1.0, 0.3, 0.0)
    bool is_water = abs(inColor.r - 0.1) < 0.01 && abs(inColor.g - 0.4) < 0.01 && abs(inColor.b - 0.8) < 0.01;
    bool is_lava  = abs(inColor.r - 1.0) < 0.01 && abs(inColor.g - 0.3) < 0.01 && abs(inColor.b - 0.0) < 0.01;
    
    if (is_water || is_lava) {
        pos.y += sin(pc.time * 2.0 + pos.x + pos.z) * 0.1;
    }

    gl_Position = pc.mvp * vec4(pos, 1.0);
    fragColor = inColor;
    fragWorldPos = pos;
    fragNormal = inNormal;
}
