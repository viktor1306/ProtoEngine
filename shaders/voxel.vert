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
// ---------------------------------------------------------------------------

layout(location = 0) in uvec4 inPosAndFace;   // x, y, z, faceID
layout(location = 1) in uvec4 inAoAndPalette;  // ao, reserved, paletteIdx_lo, paletteIdx_hi

layout(location = 0) out vec3  fragColor;
layout(location = 1) out vec3  fragNormal;
layout(location = 2) out float fragAO;
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
// Hardcoded block palette (indexed by paletteIdx 0-15)
// Index 0 = AIR (should never be rendered)
// Index 1 = Stone  (gray)
// Index 2 = Dirt   (brown)
// Index 3 = Grass  (green)
// Index 4 = Sand   (yellow)
// Index 5 = Water  (blue)
// Index 6 = Wood   (dark brown)
// Index 7 = Leaves (dark green)
// Index 8 = Snow   (white)
// Index 9 = Lava   (orange-red)
// ---------------------------------------------------------------------------
const vec3 k_palette[16] = vec3[16](
    vec3(0.0,  0.0,  0.0 ),  // 0  AIR (black — should not appear)
    vec3(0.50, 0.50, 0.50),  // 1  Stone
    vec3(0.55, 0.35, 0.18),  // 2  Dirt
    vec3(0.30, 0.65, 0.20),  // 3  Grass
    vec3(0.85, 0.80, 0.50),  // 4  Sand
    vec3(0.20, 0.40, 0.80),  // 5  Water
    vec3(0.40, 0.25, 0.10),  // 6  Wood
    vec3(0.15, 0.45, 0.10),  // 7  Leaves
    vec3(0.90, 0.92, 0.95),  // 8  Snow
    vec3(0.90, 0.30, 0.05),  // 9  Lava
    vec3(0.70, 0.70, 0.70),  // 10 Cobblestone
    vec3(0.95, 0.90, 0.60),  // 11 Sandstone
    vec3(0.60, 0.10, 0.10),  // 12 Brick
    vec3(0.20, 0.20, 0.20),  // 13 Coal Ore
    vec3(0.80, 0.70, 0.20),  // 14 Gold Ore
    vec3(0.40, 0.60, 0.80)   // 15 Diamond Ore
);

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

    // Fetch block color from hardcoded palette
    vec3 blockColor = k_palette[paletteIdx];

    // Apply AO darkening
    float aoFactor = k_aoFactors[clamp(ao, 0u, 3u)];

    // Outputs
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
    fragColor    = blockColor;
    fragNormal   = k_normals[clamp(faceID, 0u, 5u)];
    fragAO       = aoFactor;
    fragWorldPos = worldPos;
}
