#include "Chunk.hpp"
#include <cstdlib>  // rand

namespace world {

// ---------------------------------------------------------------------------
// Heightmap noise — deterministic hash-based pseudo-noise
// Returns height in [minH, minH + range)
// ---------------------------------------------------------------------------
static int hashNoise(int wx, int wz, int seed) {
    uint32_t h = static_cast<uint32_t>(wx * 1619 + wz * 31337 + seed * 1013904223);
    h = h * 1664525u + 1013904223u;
    h ^= (h >> 16);
    return static_cast<int>(h & 0xFFFF);
}

static float smoothNoise(int wx, int wz, int seed, int scale) {
    // Use floor division to handle negative coordinates correctly
    int gx = (wx >= 0) ? (wx / scale) : ((wx - scale + 1) / scale);
    int gz = (wz >= 0) ? (wz / scale) : ((wz - scale + 1) / scale);
    // Fractional part always in [0, 1)
    float fx = static_cast<float>(wx - gx * scale) / static_cast<float>(scale);
    float fz = static_cast<float>(wz - gz * scale) / static_cast<float>(scale);
    // Smoothstep
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);

    float h00 = hashNoise(gx,     gz,     seed) / 65535.0f;
    float h10 = hashNoise(gx + 1, gz,     seed) / 65535.0f;
    float h01 = hashNoise(gx,     gz + 1, seed) / 65535.0f;
    float h11 = hashNoise(gx + 1, gz + 1, seed) / 65535.0f;

    return h00 * (1-fx)*(1-fz) + h10 * fx*(1-fz) +
           h01 * (1-fx)*fz     + h11 * fx*fz;
}

static int getTerrainHeight(int wx, int wz, int seed) {
    float n  = smoothNoise(wx, wz, seed,     8) * 0.5f;
    n       += smoothNoise(wx, wz, seed + 1, 4) * 0.3f;
    n       += smoothNoise(wx, wz, seed + 2, 2) * 0.2f;
    return 4 + static_cast<int>(n * 20.0f); // height 4..24
}

// ---------------------------------------------------------------------------
// Chunk
// ---------------------------------------------------------------------------
Chunk::Chunk(int cx, int cy, int cz)
    : m_cx(cx), m_cy(cy), m_cz(cz)
{
    // Zero-init: all voxels = VOXEL_AIR (raw=0)
}

void Chunk::setVoxel(int x, int y, int z, VoxelData v) {
    m_voxels[idx(x, y, z)] = v;
    m_isDirty = true;
}

VoxelData Chunk::getVoxel(int x, int y, int z) const {
    return m_voxels[idx(x, y, z)];
}

void Chunk::fill(VoxelData v) {
    for (auto& vox : m_voxels) vox = v;
    m_isDirty = true;
}

void Chunk::fillTerrain(int seed) {
    // Palette indices: 1=stone, 2=dirt, 3=grass, 0=air
    const VoxelData stone = VoxelData::make(1, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData dirt  = VoxelData::make(2, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData grass = VoxelData::make(3, 255, 0, VOXEL_FLAG_SOLID);

    int worldBaseY = m_cy * CHUNK_SIZE;

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int wx = m_cx * CHUNK_SIZE + x;
            int wz = m_cz * CHUNK_SIZE + z;
            int terrainH = getTerrainHeight(wx, wz, seed);

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int wy = worldBaseY + y;
                VoxelData v = VOXEL_AIR;
                if      (wy < terrainH - 3) v = stone;
                else if (wy < terrainH - 1) v = dirt;
                else if (wy == terrainH - 1) v = grass;
                m_voxels[idx(x, y, z)] = v;
            }
        }
    }
    m_isDirty = true;
}

void Chunk::fillRandom(int seed) {
    const VoxelData stone = VoxelData::make(1, 255, 0, VOXEL_FLAG_SOLID);
    uint32_t rng = static_cast<uint32_t>(seed ^ 0xDEADBEEF);
    for (auto& v : m_voxels) {
        rng = rng * 1664525u + 1013904223u;
        v = ((rng >> 16) & 3) ? stone : VOXEL_AIR;
    }
    m_isDirty = true;
}

// ---------------------------------------------------------------------------
// isAirAt — checks local or neighbour chunk
// Face order: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
// ---------------------------------------------------------------------------
bool Chunk::isAirAt(int x, int y, int z,
                    const std::array<const Chunk*, 6>& neighbors) const
{
    // In-bounds: check local voxel
    if (x >= 0 && x < CHUNK_SIZE &&
        y >= 0 && y < CHUNK_SIZE &&
        z >= 0 && z < CHUNK_SIZE)
    {
        return !m_voxels[idx(x, y, z)].isSolid();
    }

    // Out-of-bounds: AO samples can exit on multiple axes simultaneously.
    // Clamp each coordinate to [0, CHUNK_SIZE-1] for cross-chunk AO queries.
    // This is an approximation — for AO we just need "is there a solid block
    // roughly in that direction", so clamping is acceptable.
    int cx = (x < 0) ? 0 : (x >= CHUNK_SIZE ? CHUNK_SIZE - 1 : x);
    int cy = (y < 0) ? 0 : (y >= CHUNK_SIZE ? CHUNK_SIZE - 1 : y);
    int cz = (z < 0) ? 0 : (z >= CHUNK_SIZE ? CHUNK_SIZE - 1 : z);

    // Determine primary out-of-bounds axis for neighbour lookup
    // Priority: X > Y > Z (arbitrary but consistent)
    const Chunk* nb = nullptr;
    int lx = cx, ly = cy, lz = cz;

    if (x >= CHUNK_SIZE)      { nb = neighbors[0]; lx = x - CHUNK_SIZE; if (lx >= CHUNK_SIZE) lx = CHUNK_SIZE-1; }
    else if (x < 0)           { nb = neighbors[1]; lx = x + CHUNK_SIZE; if (lx < 0) lx = 0; }
    else if (y >= CHUNK_SIZE) { nb = neighbors[2]; ly = y - CHUNK_SIZE; if (ly >= CHUNK_SIZE) ly = CHUNK_SIZE-1; }
    else if (y < 0)           { nb = neighbors[3]; ly = y + CHUNK_SIZE; if (ly < 0) ly = 0; }
    else if (z >= CHUNK_SIZE) { nb = neighbors[4]; lz = z - CHUNK_SIZE; if (lz >= CHUNK_SIZE) lz = CHUNK_SIZE-1; }
    else if (z < 0)           { nb = neighbors[5]; lz = z + CHUNK_SIZE; if (lz < 0) lz = 0; }

    if (!nb) return true; // no neighbour → treat as AIR
    // Clamp local coords to valid range before indexing
    lx = (lx < 0) ? 0 : (lx >= CHUNK_SIZE ? CHUNK_SIZE-1 : lx);
    ly = (ly < 0) ? 0 : (ly >= CHUNK_SIZE ? CHUNK_SIZE-1 : ly);
    lz = (lz < 0) ? 0 : (lz >= CHUNK_SIZE ? CHUNK_SIZE-1 : lz);
    return !nb->m_voxels[idx(lx, ly, lz)].isSolid();
}

// ---------------------------------------------------------------------------
// computeAO — vertex ambient occlusion (0=dark, 3=bright)
// ---------------------------------------------------------------------------
uint8_t Chunk::computeAO(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 0;
    return static_cast<uint8_t>(3 - static_cast<int>(side1)
                                   - static_cast<int>(side2)
                                   - static_cast<int>(corner));
}

// ---------------------------------------------------------------------------
// generateMesh — Hidden Face Culling
//
// For each solid voxel, check 6 neighbours.
// If neighbour is AIR → emit a quad (4 VoxelVertex + 6 indices).
// VoxelVertex coordinates are LOCAL (0-31) — shader adds chunkOffset.
//
// Face vertex layout (CCW winding, front face = counter-clockwise):
//   v0---v3
//   |  \ |
//   v1---v2
// Indices: 0,1,2, 0,2,3
// ---------------------------------------------------------------------------

// Per-face: 4 corner offsets (dx,dy,dz) relative to voxel origin
// Order: v0, v1, v2, v3 — CCW winding when viewed from OUTSIDE (from the normal direction)
// Vulkan: VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_CULL_MODE_BACK_BIT
// Right-hand rule: cross(v1-v0, v2-v0) must point toward the face normal
// Per-face: 4 corner offsets (dx,dy,dz) relative to voxel origin
// Order: v0,v1,v2,v3 — CCW winding when viewed from OUTSIDE (from the normal direction)
// Vulkan: VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_CULL_MODE_BACK_BIT
// Verification: cross(v1-v0, v2-v0) must point in the face normal direction.
//
// Coordinate system: X=right, Y=up, Z=toward viewer (right-handed)
// When looking from outside along -N direction, CCW = left-hand turn v0→v1→v2
// Verified with cross(v1-v0, v2-v0) = face normal for each face.
static constexpr int8_t k_faceCorners[6][4][3] = {
    // +X: cross((0,0,-1),(0,1,-1)) = (1,0,0) ✓
    {{1,0,1},{1,0,0},{1,1,0},{1,1,1}},
    // -X: cross((0,0,1),(0,1,1)) = (-1,0,0) ✓
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
    // +Y: v0=(1,1,0),v1=(0,1,0),v2=(0,1,1) → cross((-1,0,0),(-1,0,1))=(0,1,0) ✓
    {{1,1,0},{0,1,0},{0,1,1},{1,1,1}},
    // -Y: v0=(0,0,0),v1=(1,0,0),v2=(1,0,1) → cross((1,0,0),(1,0,1))=(0,-1,0) ✓
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    // +Z: v0=(0,0,1),v1=(1,0,1),v2=(1,1,1) → cross((1,0,0),(1,1,0))=(0,0,1) ✓
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    // -Z: v0=(1,0,0),v1=(0,0,0),v2=(0,1,0) → cross((-1,0,0),(-1,1,0))=(0,0,-1) ✓
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
};

// AO sample offsets for each face corner (side1, side2, corner)
// Matches the new k_faceCorners order exactly.
// For each corner vertex, we sample 2 edge-adjacent blocks + 1 diagonal block.
// All offsets are relative to the voxel origin (not the vertex position).
// AO sample offsets matching the new k_faceCorners exactly.
// For each corner vertex, sample 2 edge-adjacent + 1 diagonal neighbour block.
// Offsets are relative to the voxel origin.
// Format: [face][corner][sample] = {dx, dy, dz}
// sample[0]=side1, sample[1]=side2, sample[2]=corner
static constexpr int8_t k_aoSamples[6][4][3][3] = {
    // +X face: corners {(1,0,1),(1,0,0),(1,1,0),(1,1,1)}
    {
        {{1,-1,0},{1,0, 1},{1,-1, 1}}, // v0 (1,0,1): below + front
        {{1,-1,0},{1,0,-1},{1,-1,-1}}, // v1 (1,0,0): below + back
        {{1, 1,0},{1,0,-1},{1, 1,-1}}, // v2 (1,1,0): above + back
        {{1, 1,0},{1,0, 1},{1, 1, 1}}, // v3 (1,1,1): above + front
    },
    // -X face: corners {(0,0,0),(0,0,1),(0,1,1),(0,1,0)}
    {
        {{-1,-1,0},{-1,0,-1},{-1,-1,-1}}, // v0 (0,0,0): below + back
        {{-1,-1,0},{-1,0, 1},{-1,-1, 1}}, // v1 (0,0,1): below + front
        {{-1, 1,0},{-1,0, 1},{-1, 1, 1}}, // v2 (0,1,1): above + front
        {{-1, 1,0},{-1,0,-1},{-1, 1,-1}}, // v3 (0,1,0): above + back
    },
    // +Y face: corners {(1,1,0),(0,1,0),(0,1,1),(1,1,1)}
    {
        {{ 1,1,0},{0,1,-1},{ 1,1,-1}}, // v0 (1,1,0): right + back
        {{-1,1,0},{0,1,-1},{-1,1,-1}}, // v1 (0,1,0): left + back
        {{-1,1,0},{0,1, 1},{-1,1, 1}}, // v2 (0,1,1): left + front
        {{ 1,1,0},{0,1, 1},{ 1,1, 1}}, // v3 (1,1,1): right + front
    },
    // -Y face: corners {(0,0,0),(1,0,0),(1,0,1),(0,0,1)}
    {
        {{-1,-1,0},{0,-1,-1},{-1,-1,-1}}, // v0 (0,0,0): left + back
        {{ 1,-1,0},{0,-1,-1},{ 1,-1,-1}}, // v1 (1,0,0): right + back
        {{ 1,-1,0},{0,-1, 1},{ 1,-1, 1}}, // v2 (1,0,1): right + front
        {{-1,-1,0},{0,-1, 1},{-1,-1, 1}}, // v3 (0,0,1): left + front
    },
    // +Z face: corners {(0,0,1),(1,0,1),(1,1,1),(0,1,1)}
    {
        {{-1,0,1},{0,-1,1},{-1,-1,1}}, // v0 (0,0,1): left + below
        {{ 1,0,1},{0,-1,1},{ 1,-1,1}}, // v1 (1,0,1): right + below
        {{ 1,0,1},{0, 1,1},{ 1, 1,1}}, // v2 (1,1,1): right + above
        {{-1,0,1},{0, 1,1},{-1, 1,1}}, // v3 (0,1,1): left + above
    },
    // -Z face: corners {(1,0,0),(0,0,0),(0,1,0),(1,1,0)}
    {
        {{ 1,0,-1},{0,-1,-1},{ 1,-1,-1}}, // v0 (1,0,0): right + below
        {{-1,0,-1},{0,-1,-1},{-1,-1,-1}}, // v1 (0,0,0): left + below
        {{-1,0,-1},{0, 1,-1},{-1, 1,-1}}, // v2 (0,1,0): left + above
        {{ 1,0,-1},{0, 1,-1},{ 1, 1,-1}}, // v3 (1,1,0): right + above
    },
};

// Neighbour direction per faceID
static constexpr int8_t k_faceDir[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

VoxelMeshData Chunk::generateMesh(const std::array<const Chunk*, 6>& neighbors) const
{
    VoxelMeshData mesh;
    mesh.vertices.reserve(4096);
    mesh.indices.reserve(6144);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                const VoxelData& vox = m_voxels[idx(x, y, z)];
                if (!vox.isSolid()) continue;

                uint16_t palIdx = vox.getPaletteIndex();

                for (int f = 0; f < 6; ++f) {
                    int nx = x + k_faceDir[f][0];
                    int ny = y + k_faceDir[f][1];
                    int nz = z + k_faceDir[f][2];

                    if (!isAirAt(nx, ny, nz, neighbors)) continue;

                    // Emit quad — 4 vertices
                    uint32_t baseIdx = static_cast<uint32_t>(mesh.vertices.size());

                    for (int c = 0; c < 4; ++c) {
                        int vx = x + k_faceCorners[f][c][0];
                        int vy = y + k_faceCorners[f][c][1];
                        int vz = z + k_faceCorners[f][c][2];

                        // AO: sample 3 neighbours around this corner
                        const auto& ao = k_aoSamples[f][c];
                        bool s1 = !isAirAt(x + ao[0][0], y + ao[0][1], z + ao[0][2], neighbors);
                        bool s2 = !isAirAt(x + ao[1][0], y + ao[1][1], z + ao[1][2], neighbors);
                        bool cr = !isAirAt(x + ao[2][0], y + ao[2][1], z + ao[2][2], neighbors);
                        uint8_t aoVal = computeAO(s1, s2, cr);

                        VoxelVertex vert{};
                        vert.x          = static_cast<uint8_t>(vx);
                        vert.y          = static_cast<uint8_t>(vy);
                        vert.z          = static_cast<uint8_t>(vz);
                        vert.faceID     = static_cast<uint8_t>(f);
                        vert.ao         = aoVal;
                        vert.reserved   = 0;
                        vert.paletteIdx = palIdx;
                        mesh.vertices.push_back(vert);
                    }

                    // 2 triangles (CCW): 0,1,2 and 0,2,3
                    // AO-correct flipping: if ao[0]+ao[2] < ao[1]+ao[3], flip diagonal
                    uint8_t ao0 = mesh.vertices[baseIdx + 0].ao;
                    uint8_t ao1 = mesh.vertices[baseIdx + 1].ao;
                    uint8_t ao2 = mesh.vertices[baseIdx + 2].ao;
                    uint8_t ao3 = mesh.vertices[baseIdx + 3].ao;

                    if (ao0 + ao2 < ao1 + ao3) {
                        // Flip diagonal for better AO interpolation
                        mesh.indices.push_back(baseIdx + 1);
                        mesh.indices.push_back(baseIdx + 2);
                        mesh.indices.push_back(baseIdx + 3);
                        mesh.indices.push_back(baseIdx + 1);
                        mesh.indices.push_back(baseIdx + 3);
                        mesh.indices.push_back(baseIdx + 0);
                    } else {
                        mesh.indices.push_back(baseIdx + 0);
                        mesh.indices.push_back(baseIdx + 1);
                        mesh.indices.push_back(baseIdx + 2);
                        mesh.indices.push_back(baseIdx + 0);
                        mesh.indices.push_back(baseIdx + 2);
                        mesh.indices.push_back(baseIdx + 3);
                    }
                }
            }
        }
    }

    return mesh;
}

} // namespace world
