#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 outColor;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    outColor = inColor;
}
