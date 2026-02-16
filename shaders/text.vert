#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

layout(push_constant) uniform PushConstants {
    vec2 scale;     // Screen scale corrections
    vec2 translate; // Screen offset
    vec3 color;     // Text Color
    uint textureID; // Font Atlas ID
} pc;

void main() {
    gl_Position = vec4(inPosition * pc.scale + pc.translate, 0.0, 1.0);
    fragUV = inUV;
}
