#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    float visionRadius;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec3 fragNormal;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragWorldPos = inPosition;
    fragNormal = inNormal;
}
