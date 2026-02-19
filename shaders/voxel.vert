#version 450

// ---------------------------------------------------------------------------
// Voxel Vertex Shader
// Accepts compressed VoxelVertex (8 bytes per vertex):
//   location 0: uvec4(x, y, z, faceID)
//   location 1: uvec4(ao, reserved, paletteIdx_lo, paletteIdx_hi)
//
// Chunk world position is passed via push constants (chunkOffset).
// Block color is resolved from a hardcoded palette in the shader.
// AO is decoded and applied as a soft darkening factor.
//
// NOTE: fragNormal and fragAO use 'flat' qualifier — no interpolation.
//   This is correct for voxel rendering: all vertices of a greedy-merged
//   quad share the same face normal and AO should not bleed across the quad.
//   Without 'flat', large greedy quads show gradient artifacts (dark stripes).
// ---------------------------------------------------------------------------

layout(location = 0) in uvec4 inPosAndFace;   // x, y, z, faceID
layout(location = 1) in uvec4 inAoAndPalette;  // ao, reserved, paletteIdx_lo, paletteIdx_hi

layout(location = 0) out vec3  fragColor;
layout(location = 1) flat out vec3  fragNormal;  // flat: no interpolation across triangle
layout(location = 2) flat out float fragAO;      // flat: per-vertex AO, not interpolated
layout(location = 3) out vec3  fragWorldPos;

// ---------------------------------------------------------------------------
// Push Constants (matches VoxelPushConstants in main.cpp — 144 bytes)
// ---------------------------------------------------------------------------
layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    mat4  lightSpaceMatrix;
    vec3  chunkOffset;
    float _pad;
} pc;

// ---------------------------------------------------------------------------
// Block palette UBO (indexed by paletteIdx 0-15)
// ---------------------------------------------------------------------------
layout(set = 1, binding = 2) uniform PaletteBuffer {
    vec4 colors[16];
} palette;

// ---------------------------------------------------------------------------
// Face normals table (indexed by faceID 0-5)
// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
// ---------------------------------------------------------------------------
const vec3 k_normals[6] = vec3[6](
    vec3( 1, 0, 0), vec3(-1, 0, 0),
    vec3( 0, 1, 0), vec3( 0,-1, 0),
    vec3( 0, 0, 1), vec3( 0, 0,-1)
);

// ---------------------------------------------------------------------------
// AO darkening factors (0=fully occluded, 3=fully lit)
// Smooth curve: 0→0.4, 1→0.6, 2→0.8, 3→1.0
// ---------------------------------------------------------------------------
const float k_aoFactors[4] = float[4](0.4, 0.6, 0.8, 1.0);

void main() {
    // Unpack position
    float lx     = float(inPosAndFace.x);
    float ly     = float(inPosAndFace.y);
    float lz     = float(inPosAndFace.z);
    uint  faceID = inPosAndFace.w;

    // World position = chunk offset + local position
    vec3 worldPos = pc.chunkOffset + vec3(lx, ly, lz);

    // Unpack AO and palette index
    uint ao         = inAoAndPalette.x;
    uint palLo      = inAoAndPalette.z;
    uint palHi      = inAoAndPalette.w;
    uint paletteIdx = (palLo | (palHi << 8u)) & 0xFu; // clamp to 0-15

    // Fetch block color from UBO palette
    vec3 blockColor = palette.colors[paletteIdx].rgb;

    // Apply AO darkening
    float aoFactor = k_aoFactors[clamp(ao, 0u, 3u)];

    // Outputs
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
    fragColor    = blockColor;
    fragNormal   = k_normals[clamp(faceID, 0u, 5u)];  // flat — provoking vertex value
    fragAO       = aoFactor;                            // flat — no gradient across quad
    fragWorldPos = worldPos;
}
