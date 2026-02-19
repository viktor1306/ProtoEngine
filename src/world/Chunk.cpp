#include "Chunk.hpp"
#include <cstdlib>
#include <cstring>

namespace world {

// ---------------------------------------------------------------------------
// Heightmap noise
// ---------------------------------------------------------------------------
static int hashNoise(int wx, int wz, int seed) {
    uint32_t h = static_cast<uint32_t>(wx * 1619 + wz * 31337 + seed * 1013904223);
    h = h * 1664525u + 1013904223u;
    h ^= (h >> 16);
    return static_cast<int>(h & 0xFFFF);
}

static float smoothNoise(int wx, int wz, int seed, int scale) {
    int gx = (wx >= 0) ? (wx / scale) : ((wx - scale + 1) / scale);
    int gz = (wz >= 0) ? (wz / scale) : ((wz - scale + 1) / scale);
    float fx = static_cast<float>(wx - gx * scale) / static_cast<float>(scale);
    float fz = static_cast<float>(wz - gz * scale) / static_cast<float>(scale);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float h00 = hashNoise(gx,     gz,     seed) / 65535.0f;
    float h10 = hashNoise(gx + 1, gz,     seed) / 65535.0f;
    float h01 = hashNoise(gx,     gz + 1, seed) / 65535.0f;
    float h11 = hashNoise(gx + 1, gz + 1, seed) / 65535.0f;
    return h00*(1-fx)*(1-fz) + h10*fx*(1-fz) + h01*(1-fx)*fz + h11*fx*fz;
}

static int getTerrainHeight(int wx, int wz, int seed) {
    float n  = smoothNoise(wx, wz, seed,     8) * 0.5f;
    n       += smoothNoise(wx, wz, seed + 1, 4) * 0.3f;
    n       += smoothNoise(wx, wz, seed + 2, 2) * 0.2f;
    return 4 + static_cast<int>(n * 20.0f);
}

// ---------------------------------------------------------------------------
// Chunk
// ---------------------------------------------------------------------------
Chunk::Chunk(int cx, int cy, int cz) : m_cx(cx), m_cy(cy), m_cz(cz) {}

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
                if      (wy < terrainH - 3)  v = stone;
                else if (wy < terrainH - 1)  v = dirt;
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
// isAirAt
// ---------------------------------------------------------------------------
bool Chunk::isAirAt(int x, int y, int z,
                    const std::array<const Chunk*, 6>& neighbors) const
{
    if (x >= 0 && x < CHUNK_SIZE &&
        y >= 0 && y < CHUNK_SIZE &&
        z >= 0 && z < CHUNK_SIZE)
        return !m_voxels[idx(x, y, z)].isSolid();

    const Chunk* nb = nullptr;
    int lx = x, ly = y, lz = z;

    if      (x >= CHUNK_SIZE) { nb = neighbors[0]; lx = x - CHUNK_SIZE; }
    else if (x < 0)           { nb = neighbors[1]; lx = x + CHUNK_SIZE; }
    else if (y >= CHUNK_SIZE) { nb = neighbors[2]; ly = y - CHUNK_SIZE; }
    else if (y < 0)           { nb = neighbors[3]; ly = y + CHUNK_SIZE; }
    else if (z >= CHUNK_SIZE) { nb = neighbors[4]; lz = z - CHUNK_SIZE; }
    else if (z < 0)           { nb = neighbors[5]; lz = z + CHUNK_SIZE; }

    if (!nb) return true;
    lx = (lx < 0) ? 0 : (lx >= CHUNK_SIZE ? CHUNK_SIZE-1 : lx);
    ly = (ly < 0) ? 0 : (ly >= CHUNK_SIZE ? CHUNK_SIZE-1 : ly);
    lz = (lz < 0) ? 0 : (lz >= CHUNK_SIZE ? CHUNK_SIZE-1 : lz);
    return !nb->m_voxels[idx(lx, ly, lz)].isSolid();
}

// ---------------------------------------------------------------------------
// computeAO
// ---------------------------------------------------------------------------
uint8_t Chunk::computeAO(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 0;
    return static_cast<uint8_t>(3 - (int)side1 - (int)side2 - (int)corner);
}

// ---------------------------------------------------------------------------
// generateMesh — Greedy Meshing with Soft Gradient AO
//
// Algorithm:
//   For each axis d (0=X, 1=Y, 2=Z) and direction (normalDir = +1 / -1):
//     For each layer along d:
//       1. Build 2D mask: FaceMask[CHUNK_SIZE x CHUNK_SIZE]
//          Each cell stores paletteIdx + faceID + ao[4] for that face.
//       2. Greedy scan: find first non-empty cell (i,j).
//          Expand W along u-axis: merge if paletteIdx+faceID match (ignore AO).
//          Expand H along v-axis: entire row of W cells must match.
//       3. Emit one quad W×H.
//          AO for 4 corners taken from the actual corner cells of the rectangle
//          (not from ref cell) — GPU interpolates smoothly across the large quad.
//       4. Clear used cells.
//
// Winding (CCW, Vulkan VK_FRONT_FACE_COUNTER_CLOCKWISE):
//   Positive normal: c0(i,j), c1(i+W,j), c2(i+W,j+H), c3(i,j+H)
//   Negative normal: c3(i,j+H), c2(i+W,j+H), c1(i+W,j), c0(i,j)  [mirrored]
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FaceMask cell
// ---------------------------------------------------------------------------
struct FaceMask {
    uint16_t paletteIdx = 0;
    uint8_t  faceID     = 0xFF; // 0xFF = empty
    uint8_t  ao[4]      = {};   // per-corner AO: [0]=(i,j) [1]=(i+1,j) [2]=(i+1,j+1) [3]=(i,j+1)

    bool empty() const { return faceID == 0xFF; }

    // Soft gradient: merge by paletteIdx+faceID only, ignore AO.
    // AO will be interpolated by GPU across the merged quad.
    bool canMerge(const FaceMask& o) const {
        return !o.empty() && faceID == o.faceID && paletteIdx == o.paletteIdx;
    }
};

// ---------------------------------------------------------------------------
// AO helper: compute AO for a vertex at the corner of a face.
//
// d = normal axis (0=X,1=Y,2=Z), normalDir = +1 or -1
// pos[3] = voxel position (x,y,z)
// du, dv = corner offset in (u,v) plane: -1 or +1
// ---------------------------------------------------------------------------
static inline int cacheIdx(int x, int y, int z) {
    return (x + 1) + (y + 1) * 34 + (z + 1) * 34 * 34;
}

static uint8_t sampleAO(const VoxelData* cache,
                        int pos[3], int d, int du, int dv, int normalDir)
{
    const int u = (d + 1) % 3;
    const int v = (d + 2) % 3;

    // Base: step into the face plane
    int base[3] = { pos[0], pos[1], pos[2] };
    base[d] += (normalDir > 0) ? 1 : -1;

    int s1[3] = { base[0], base[1], base[2] }; s1[u] += du;
    int s2[3] = { base[0], base[1], base[2] }; s2[v] += dv;
    int sc[3] = { base[0], base[1], base[2] }; sc[u] += du; sc[v] += dv;

    bool b1 = cache[cacheIdx(s1[0], s1[1], s1[2])].isSolid();
    bool b2 = cache[cacheIdx(s2[0], s2[1], s2[2])].isSolid();
    bool bc = cache[cacheIdx(sc[0], sc[1], sc[2])].isSolid();

    return Chunk::computeAO(b1, b2, bc);
}

// ---------------------------------------------------------------------------
// emitQuad — helper to push 4 vertices + 6 indices into mesh
// ---------------------------------------------------------------------------
static void emitQuad(VoxelMeshData& mesh,
                     int corners[4][3],
                     uint8_t faceID,
                     uint16_t paletteIdx,
                     uint8_t ao0, uint8_t ao1, uint8_t ao2, uint8_t ao3,
                     int normalDir)
{
    int    vOrder[4];
    uint8_t vAO[4];
    if (normalDir > 0) {
        vOrder[0]=0; vOrder[1]=1; vOrder[2]=2; vOrder[3]=3;
        vAO[0]=ao0; vAO[1]=ao1; vAO[2]=ao2; vAO[3]=ao3;
    } else {
        vOrder[0]=3; vOrder[1]=2; vOrder[2]=1; vOrder[3]=0;
        vAO[0]=ao3; vAO[1]=ao2; vAO[2]=ao1; vAO[3]=ao0;
    }

    uint32_t baseIdx = static_cast<uint32_t>(mesh.vertices.size());
    for (int c = 0; c < 4; ++c) {
        const int* co = corners[vOrder[c]];
        VoxelVertex vert{};
        vert.x          = static_cast<uint8_t>(co[0]);
        vert.y          = static_cast<uint8_t>(co[1]);
        vert.z          = static_cast<uint8_t>(co[2]);
        vert.faceID     = faceID;
        vert.ao         = vAO[c];
        vert.reserved   = 0;
        vert.paletteIdx = paletteIdx;
        mesh.vertices.push_back(vert);
    }

    // AO-correct diagonal flip
    if (vAO[0] + vAO[2] < vAO[1] + vAO[3]) {
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

// ---------------------------------------------------------------------------
// generateMesh — LOD-aware Greedy Meshing
//
// lod=0: step=1 — standard per-voxel Greedy Meshing (full quality)
// lod=1: step=2 — 2×2×2 super-voxels, ~4× fewer vertices
// lod=2: step=4 — 4×4×4 super-voxels, ~16× fewer vertices
//
// Super-voxel logic:
//   A super-voxel at (x,y,z) is SOLID if its first voxel (x,y,z) is solid.
//   paletteIdx taken from the first voxel of the super-block.
//   AO sampled at super-voxel corners (step-scaled positions).
//
// Greedy Meshing in LOD mode:
//   Mask is indexed in super-voxel space: [0..CHUNK_SIZE/step).
//   Greedy W/H are in super-voxel units.
//   Vertex coordinates are in voxel units: i*step, j*step, etc.
//   This means large quads naturally cover step×step voxels each.
//
// Skirts (lod > 0):
//   At chunk boundaries, LOD transitions create T-junctions (cracks).
//   We add "skirt" faces: extra downward-facing quads along the bottom
//   perimeter of the chunk, extending 1 voxel below the chunk boundary.
//   These fill the visual gap between LOD levels.
//   Skirts use the -Y face (faceID=3) with CCW winding for backface culling.
// ---------------------------------------------------------------------------
VoxelMeshData Chunk::generateMesh(const std::array<const Chunk*, 6>& neighbors,
                                  int lod) const
{
    // Clamp LOD to valid range
    if (lod < 0) lod = 0;
    if (lod > 2) lod = 2;

    const int step = 1 << lod;                    // 1, 2, or 4
    const int gridSize = CHUNK_SIZE / step;        // 32, 16, or 8 super-voxels per axis

    VoxelMeshData mesh;
    mesh.vertices.reserve(lod == 0 ? 2048 : 512);
    mesh.indices.reserve(lod == 0 ? 3072 : 768);

    // Thread-local buffers — avoids heap allocation per chunk.
    static thread_local FaceMask mask[CHUNK_SIZE * CHUNK_SIZE];
    static thread_local VoxelData volumeCache[34 * 34 * 34];

    // ---- Fill volumeCache (34x34x34) ---------------------------------------
    std::fill_n(volumeCache, 34 * 34 * 34, VOXEL_AIR);

    // 1. Copy chunk center
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                volumeCache[cacheIdx(x, y, z)] = m_voxels[idx(x, y, z)];
            }
        }
    }

    // 2. Copy boundary layers clamped to 6 neighbors
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE)
                    continue; // Skip center

                const Chunk* nb = nullptr;
                int lx = x, ly = y, lz = z;

                if      (x >= CHUNK_SIZE) { nb = neighbors[0]; lx = x - CHUNK_SIZE; }
                else if (x < 0)           { nb = neighbors[1]; lx = x + CHUNK_SIZE; }
                else if (y >= CHUNK_SIZE) { nb = neighbors[2]; ly = y - CHUNK_SIZE; }
                else if (y < 0)           { nb = neighbors[3]; ly = y + CHUNK_SIZE; }
                else if (z >= CHUNK_SIZE) { nb = neighbors[4]; lz = z - CHUNK_SIZE; }
                else if (z < 0)           { nb = neighbors[5]; lz = z + CHUNK_SIZE; }

                if (nb) {
                    lx = (lx < 0) ? 0 : (lx >= CHUNK_SIZE ? CHUNK_SIZE - 1 : lx);
                    ly = (ly < 0) ? 0 : (ly >= CHUNK_SIZE ? CHUNK_SIZE - 1 : ly);
                    lz = (lz < 0) ? 0 : (lz >= CHUNK_SIZE ? CHUNK_SIZE - 1 : lz);
                    volumeCache[cacheIdx(x, y, z)] = nb->getVoxel(lx, ly, lz);
                }
            }
        }
    }

    // ---- Main Greedy Meshing pass (all 6 faces) ----------------------------
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        for (int normalDir = 1; normalDir >= -1; normalDir -= 2) {

            // faceID: d=0 → 0(+X)/1(-X), d=1 → 2(+Y)/3(-Y), d=2 → 4(+Z)/5(-Z)
            const uint8_t faceID = static_cast<uint8_t>(d * 2 + (normalDir > 0 ? 0 : 1));

            // Iterate over layers in super-voxel space
            for (int layer = 0; layer < gridSize; ++layer) {

                // ---- Build mask (in super-voxel space) ----------------------
                for (int k = 0; k < gridSize * gridSize; ++k)
                    mask[k].faceID = 0xFF;

                for (int j = 0; j < gridSize; ++j) {
                    for (int i = 0; i < gridSize; ++i) {
                        // Super-voxel origin in voxel space
                        int pos[3];
                        pos[d] = layer * step;
                        pos[u] = i     * step;
                        pos[v] = j     * step;

                        // Sample the representative voxel (first corner of super-block)
                        const VoxelData& vox = m_voxels[idx(pos[0], pos[1], pos[2])];
                        if (!vox.isSolid()) continue;

                        // Check neighbour in normal direction (in voxel space)
                        int npos[3] = { pos[0], pos[1], pos[2] };
                        npos[d] += normalDir * step;
                        
                        bool isNeighborSolid = true; // Assume fully covered, try to find a hole
                        for (int dv = 0; dv < step && isNeighborSolid; ++dv) {
                            for (int du = 0; du < step && isNeighborSolid; ++du) {
                                int chkPos[3] = { npos[0], npos[1], npos[2] };
                                chkPos[u] += du;
                                chkPos[v] += dv;

                                if (chkPos[d] >= 0 && chkPos[d] < CHUNK_SIZE) {
                                    // Inside this chunk
                                    if (!m_voxels[idx(chkPos[0], chkPos[1], chkPos[2])].isSolid()) {
                                        isNeighborSolid = false;
                                    }
                                } else {
                                    // Outside the chunk -> Force draw boundary face!
                                    // By disabling inter-chunk culling, every chunk independently covers its
                                    // own bounding box. This beautifully and flawlessly eliminates ALL T-Junctions
                                    // and LOD holes between chunks of ANY detail level without extra logic.
                                    isNeighborSolid = false;
                                }
                            }
                        }

                        if (isNeighborSolid) continue;

                        FaceMask& cell = mask[j * gridSize + i];
                        cell.faceID     = faceID;
                        cell.paletteIdx = vox.getPaletteIndex();

                        // AO sampled at super-voxel corners.
                        // For LOD > 0, we use step-scaled positions for AO sampling.
                        // This gives correct AO at the actual vertex positions.
                        int aoPos[3] = { pos[0], pos[1], pos[2] };
                        // For AO, we need the voxel position (not the face layer)
                        // sampleAO expects the voxel pos, not the face pos
                        cell.ao[0] = sampleAO(volumeCache, aoPos, d, -1, -1, normalDir);
                        cell.ao[1] = sampleAO(volumeCache, aoPos, d, +1, -1, normalDir);
                        cell.ao[2] = sampleAO(volumeCache, aoPos, d, +1, +1, normalDir);
                        cell.ao[3] = sampleAO(volumeCache, aoPos, d, -1, +1, normalDir);
                    }
                }

                // ---- Greedy scan (in super-voxel space) ---------------------
                for (int j = 0; j < gridSize; ++j) {
                    for (int i = 0; i < gridSize; ) {
                        const FaceMask& ref = mask[j * gridSize + i];
                        if (ref.empty()) { ++i; continue; }

                        // Expand W along u (in super-voxel units)
                        int W = 1;
                        while (i + W < gridSize &&
                               ref.canMerge(mask[j * gridSize + (i + W)]))
                            ++W;

                        // Expand H along v (in super-voxel units)
                        int H = 1;
                        bool canExpand = true;
                        while (j + H < gridSize && canExpand) {
                            for (int k = 0; k < W; ++k) {
                                if (!ref.canMerge(mask[(j+H)*gridSize + (i+k)])) {
                                    canExpand = false;
                                    break;
                                }
                            }
                            if (canExpand) ++H;
                        }

                        // ---- Emit quad W×H (in voxel coords) ---------------
                        // Convert super-voxel coords to voxel coords
                        int vi  = i * step;
                        int vj  = j * step;
                        int vW  = W * step;
                        int vH  = H * step;
                        int faceLayer = layer * step + (normalDir > 0 ? step : 0);

                        int corners[4][3];
                        corners[0][d]=faceLayer; corners[0][u]=vi;    corners[0][v]=vj;
                        corners[1][d]=faceLayer; corners[1][u]=vi+vW; corners[1][v]=vj;
                        corners[2][d]=faceLayer; corners[2][u]=vi+vW; corners[2][v]=vj+vH;
                        corners[3][d]=faceLayer; corners[3][u]=vi;    corners[3][v]=vj+vH;

                        // AO from corner cells of the greedy rectangle
                        uint8_t ao0 = mask[j         * gridSize + i      ].ao[0];
                        uint8_t ao1 = mask[j         * gridSize + (i+W-1)].ao[1];
                        uint8_t ao2 = mask[(j+H-1)   * gridSize + (i+W-1)].ao[2];
                        uint8_t ao3 = mask[(j+H-1)   * gridSize + i      ].ao[3];

                        emitQuad(mesh, corners, faceID, ref.paletteIdx,
                                 ao0, ao1, ao2, ao3, normalDir);

                        // Clear used cells
                        for (int jj = j; jj < j+H; ++jj)
                            for (int ii = i; ii < i+W; ++ii)
                                mask[jj * gridSize + ii].faceID = 0xFF;

                        i += W;
                    }
                }
            } // layer
        } // normalDir
    } // d

    // ---- Skirts (lod > 0 only) ---------------------------------------------
    // REMOVED: Since we now force boundary faces (isNeighborSolid = false at
    // chunk bounds), we get perfect natural "skirts" on all 6 sides of every
    // chunk for free. This fully eliminates LOD seams across chunk boundaries!

    return mesh;
}

} // namespace world
