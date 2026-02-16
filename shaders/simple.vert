#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragPosLightSpace;
layout(location = 3) out vec2 fragUV;
layout(location = 4) out flat uint textureID; // Flat interpolation for ID
layout(location = 5) out vec3 fragPos;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 lightSpaceMatrix;
    uint objectIndex; // Index into ObjectBuffer
} pc;

struct ObjectData {
    mat4 model;
    vec4 color;     // Optional tint
    uint textureID;
    uint padding[3];
};

layout(std430, set = 1, binding = 1) readonly buffer ObjectBuffer {
    ObjectData objects[];
};

void main() {
    ObjectData obj = objects[pc.objectIndex];
    
    vec4 worldPos = obj.model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * worldPos;
    fragColor = vec3(1.0); // Default white
    
    fragNormal = mat3(transpose(inverse(obj.model))) * inNormal;
    fragPosLightSpace = pc.lightSpaceMatrix * worldPos;
    fragUV = inUV; // Correctly use input UVs again
    textureID = obj.textureID;
}
