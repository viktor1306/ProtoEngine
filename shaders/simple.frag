#version 450

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D shadowMap;
layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragPosLightSpace;
layout(location = 3) in vec2 fragUV;
layout(location = 4) in flat uint textureID;
layout(location = 5) in vec3 fragPos;

layout(location = 0) out vec4 outColor;

// Push Constant must match main.cpp and vertex shader layout
layout(push_constant) uniform PushConsts {
    mat4 viewProj;
    mat4 lightSpaceMatrix;
    uint objectIndex;
} pushConsts;

float ShadowCalculation(vec3 worldPos, vec3 normal) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    
    if (projCoords.z > 1.0) return 0.0;
    
    float currentDepth = projCoords.z;
    
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.0005);
    
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;
    
    return shadow;
}

void main() {
    float shadow = ShadowCalculation(fragPos, normalize(fragNormal));
    
    // Simple directional light
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 norm = normalize(fragNormal);
    
    // Diffuse + Ambient 
    float diff = max(dot(norm, lightDir), 0.0);
    float ambient = 0.3;
    
    vec4 texColor = texture(textures[nonuniformEXT(textureID)], fragUV);
    
    // Final Lighting
    vec3 lighting = (diff * (1.0 - shadow) + ambient) * texColor.rgb;

    outColor = vec4(lighting, texColor.a);
}
