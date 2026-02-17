#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

// ---------------------------------------------------------------------------
// Voxel Vertex Shader
// Accepts compressed VoxelVertex (8 bytes per vertex):
//   location 0: uvec4(x, y, z, faceID)
//   location 1: uvec4(ao, reserved, paletteIdx_lo, paletteIdx_hi)
//
// Chunk world position is passed via push constants (chunkOffset).
// Block color is fetched from a palette SSBO via BDA.
// AO is decoded and applied as a soft darkening factor.
// ---------------------------------------------------------------------------

layout(location = 0) in uvec4 inPosAndFace;   // x, y, z, faceID
layout(location = 1) in uvec4 inAoAndPalette;  // ao, reserved, paletteIdx_lo, paletteIdx_hi

layout(location = 0) out vec3  fragColor;
layout(location = 1) out vec3  fragNormal;
layout(location = 2) out float fragAO;
layout(location = 3) out vec3  fragWorldPos;

// ---------------------------------------------------------------------------
// Push Constants
// ---------------------------------------------------------------------------
layout(push_constant) uniform PushConstants {
    mat4  viewProj;
    mat4  lightSpaceMatrix;  // kept for shadow pass compatibility
    vec3  chunkOffset;       // world-space position of chunk (0,0,0) corner
    float _pad;
} pc;

// ---------------------------------------------------------------------------
// Palette SSBO — accessed via BDA (Buffer Device Address)
// Each entry: vec4(r, g, b, emissive_strength)
// ---------------------------------------------------------------------------
struct PaletteEntry {
    vec4 color;  // rgb + emissive strength in alpha
};

layout(std430, set = 1, binding = 2) readonly buffer PaletteBuffer {
    PaletteEntry palette[];
};

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
    float lx = float(inPosAndFace.x);
    float ly = float(inPosAndFace.y);
    float lz = float(inPosAndFace.z);
    uint  faceID = inPosAndFace.w;

    // World position = chunk offset + local position
    vec3 worldPos = pc.chunkOffset + vec3(lx, ly, lz);

    // Unpack AO and palette index
    uint ao         = inAoAndPalette.x;
    uint palLo      = inAoAndPalette.z;
    uint palHi      = inAoAndPalette.w;
    uint paletteIdx = palLo | (palHi << 8u);

    // Fetch block color from palette
    vec3 blockColor = palette[paletteIdx].color.rgb;

    // Apply AO darkening
    float aoFactor = k_aoFactors[clamp(ao, 0u, 3u)];

    // Outputs
    gl_Position  = pc.viewProj * vec4(worldPos, 1.0);
    fragColor    = blockColor;
    fragNormal   = k_normals[clamp(faceID, 0u, 5u)];
    fragAO       = aoFactor;
    fragWorldPos = worldPos;
}
