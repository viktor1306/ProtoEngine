#version 450

// ---------------------------------------------------------------------------
// Voxel Fragment Shader
// Receives interpolated color, normal, AO factor and world position.
// Applies simple directional lighting + AO darkening.
// No texture sampling — color comes from the palette (resolved in vertex shader).
// ---------------------------------------------------------------------------

layout(location = 0) in vec3  fragColor;
layout(location = 1) in vec3  fragNormal;
layout(location = 2) in float fragAO;
layout(location = 3) in vec3  fragWorldPos;

layout(location = 0) out vec4 outColor;

// Push constants (must match voxel.vert layout exactly)
layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    mat4  lightSpaceMatrix;
    vec3  chunkOffset;
    float _pad;
} pc;

void main() {
    // Simple directional light (sun)
    vec3  lightDir = normalize(vec3(0.6, 1.0, 0.4));
    vec3  norm     = normalize(fragNormal);
    float diff     = max(dot(norm, lightDir), 0.0);
    float ambient  = 0.25;

    // Combine diffuse + ambient, then apply AO
    // AO darkens corners/crevices (fragAO: 0.4=dark, 1.0=fully lit)
    float lighting = (ambient + diff * 0.75) * fragAO;

    // Slight face-based shading variation for visual depth (Minecraft-style)
    // Top faces (+Y) are brightest, bottom faces (-Y) are darkest
    float faceShade = 1.0;
    if (abs(norm.y - 1.0) < 0.01)       faceShade = 1.00; // top
    else if (abs(norm.y + 1.0) < 0.01)  faceShade = 0.50; // bottom
    else if (abs(norm.x) > 0.5)         faceShade = 0.80; // X sides
    else                                 faceShade = 0.70; // Z sides

    vec3 finalColor = fragColor * lighting * faceShade;

    // Gamma correction (approximate sRGB)
    finalColor = pow(clamp(finalColor, 0.0, 1.0), vec3(1.0 / 2.2));

    outColor = vec4(finalColor, 1.0);
}
