#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    mat4 lightSpaceMatrix;
    uint objectIndex;
} pc;

struct ObjectData {
    mat4 model;
    vec4 color;
    uint textureID;
    uint padding[3];
};

layout(std430, set = 1, binding = 1) readonly buffer ObjectBuffer {
    ObjectData objects[];
};

void main() {
    ObjectData obj = objects[pc.objectIndex];
    gl_Position = pc.lightSpaceMatrix * obj.model * vec4(inPosition, 1.0);
    
    // Nuclear option: Force usage of ALL inputs
    if (inNormal.x > 100.0 || inColor.x > 100.0 || inUV.x > 100.0) {
        gl_Position.x += 1.0;
    }
}
