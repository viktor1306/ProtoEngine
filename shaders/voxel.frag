#version 450

// ---------------------------------------------------------------------------
// Voxel Fragment Shader
// Receives color, normal, AO factor and world position from vertex shader.
// Applies simple directional lighting + AO darkening.
// No texture sampling — color comes from the palette (resolved in vertex shader).
//
// NOTE: fragNormal and fragAO are 'flat' — they must match the vertex shader
//   declaration exactly. 'flat' means the provoking vertex value is used for
//   the entire triangle, eliminating gradient artifacts on large greedy quads.
// ---------------------------------------------------------------------------

layout(location = 0) in vec3  fragColor;
layout(location = 1) flat in vec3  fragNormal;   // flat: no interpolation
layout(location = 2) flat in float fragAO;        // flat: no interpolation
layout(location = 3) in vec3  fragWorldPos;
layout(location = 4) in float fragFade;

layout(location = 0) out vec4 outColor;

// Push constants (must match voxel.vert layout exactly)
layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    mat4  lightSpaceMatrix;
    vec3  chunkOffset;
    float fadeProgress;
} pc;

const float bayer4[16] = float[](
    0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
   12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
    3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
   15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
);

void main() {
    // 1. Dither Fading (Crossfade/Anti-popping)
    // discard pixels if the fadeProgress is less than the threshold from the Bayer matrix
    /*
    if (fragFade < 1.0) {
        uint x = uint(gl_FragCoord.x) % 4u;
        uint y = uint(gl_FragCoord.y) % 4u;
        float threshold = bayer4[y * 4u + x];
        if (fragFade < threshold) {
            discard;
        }
    }
    */

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
