#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

// Bindless Global Textures (Set 1)
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 translate;
    vec3 color;
    uint textureID;
} pc;

const float smoothing = 1.0/16.0; // Adjustable smoothing factor

void main() {
    // Read single channel SDF
    float dist = texture(textures[nonuniformEXT(pc.textureID)], fragUV).r;
    
    // SDF Logic: 0.5 is edge. 
    // Smoothstep for anti-aliasing.
    float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, dist);
    
    if (alpha < 0.01) discard; // Optimization
    
    outColor = vec4(pc.color, alpha);
}
