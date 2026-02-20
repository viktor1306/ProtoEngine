#include "Chunk.hpp"
#include <cstdlib>
#include <cstring>
#include <bit>
#include <algorithm>
#include "../vendor/FastNoiseLite.h"

namespace world {

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
    
    FastNoiseLite noise;
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(3);
    noise.SetFrequency(0.03f);

    int worldBaseX = m_cx * CHUNK_SIZE;
    int worldBaseY = m_cy * CHUNK_SIZE;
    int worldBaseZ = m_cz * CHUNK_SIZE;
    
    int heightmap[CHUNK_SIZE][CHUNK_SIZE];
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            float nx = static_cast<float>(worldBaseX + x);
            float nz = static_cast<float>(worldBaseZ + z);
            float n = noise.GetNoise(nx, nz);
            heightmap[z][x] = 14 + static_cast<int>(n * 10.0f);
        }
    }

    // Швидка початкова ініціалізація пустим повітрям
    std::fill_n(m_voxels, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE, VOXEL_AIR);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int terrainH = heightmap[z][x];
            
            // Вираховуємо локальну максимальну висоту землі для цього чанка
            int localMaxY = terrainH - worldBaseY;
            if (localMaxY <= 0) continue; // Увесь стовпець вище рівня землі (тільки повітря)
            if (localMaxY > CHUNK_SIZE) localMaxY = CHUNK_SIZE; // Земля повністю заповнює цей стовпець
            
            for (int y = 0; y < localMaxY; ++y) {
                int wy = worldBaseY + y;
                VoxelData v;
                if      (wy < terrainH - 3)  v = stone;
                else if (wy < terrainH - 1)  v = dirt;
                else                         v = grass;
                
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
// Helper Functions for generateMesh
// ---------------------------------------------------------------------------
static constexpr int CACHE_PADDING = 4;
static constexpr int CACHE_DIM = CHUNK_SIZE + CACHE_PADDING * 2;

static inline int cacheIdx(int x, int y, int z) {
    return (x + CACHE_PADDING) + (y + CACHE_PADDING) * CACHE_DIM + (z + CACHE_PADDING) * CACHE_DIM * CACHE_DIM;
}

static uint8_t sampleAO(const VoxelData* cache,
                        int pos[3], int d, int du, int dv, int normalDir)
{
    const int u = (d + 1) % 3;
    const int v = (d + 2) % 3;

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
// generateMesh — LOD-aware Bitwise Greedy Meshing
// ---------------------------------------------------------------------------
VoxelMeshData Chunk::generateMesh(const std::array<const Chunk*, 6>& neighbors,
                                  int lod) const
{
    if (lod < 0) lod = 0;
    if (lod > 2) lod = 2;

    const int step = 1 << lod;                    
    const int gridSize = CHUNK_SIZE / step;        

    VoxelMeshData mesh;
    mesh.vertices.reserve(lod == 0 ? 2048 : 512);
    mesh.indices.reserve(lod == 0 ? 3072 : 768);

    static thread_local VoxelData volumeCache[CACHE_DIM * CACHE_DIM * CACHE_DIM];
    std::fill_n(volumeCache, CACHE_DIM * CACHE_DIM * CACHE_DIM, VOXEL_AIR);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                volumeCache[cacheIdx(x, y, z)] = m_voxels[idx(x, y, z)];
            }
        }
    }

    // Neighbors boundaries
    for (int z = -CACHE_PADDING; z < CHUNK_SIZE + CACHE_PADDING; ++z) {
        for (int y = -CACHE_PADDING; y < CHUNK_SIZE + CACHE_PADDING; ++y) {
            for (int x = -CACHE_PADDING; x < CHUNK_SIZE + CACHE_PADDING; ++x) {
                if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE)
                    continue; 

                const Chunk* nb = nullptr;
                int lx = x, ly = y, lz = z;

                if      (x >= CHUNK_SIZE) { nb = neighbors[0]; lx = x - CHUNK_SIZE; }
                else if (x < 0)           { nb = neighbors[1]; lx = x + CHUNK_SIZE; }
                else if (y >= CHUNK_SIZE) { nb = neighbors[2]; ly = y - CHUNK_SIZE; }
                else if (y < 0)           { nb = neighbors[3]; ly = y + CHUNK_SIZE; }
                else if (z >= CHUNK_SIZE) { nb = neighbors[4]; lz = z - CHUNK_SIZE; }
                else if (z < 0)           { nb = neighbors[5]; lz = z + CHUNK_SIZE; }

                if (nb) {
                    lx = std::clamp(lx, 0, CHUNK_SIZE - 1);
                    ly = std::clamp(ly, 0, CHUNK_SIZE - 1);
                    lz = std::clamp(lz, 0, CHUNK_SIZE - 1);
                    volumeCache[cacheIdx(x, y, z)] = nb->getVoxel(lx, ly, lz);
                }
            }
        }
    }

    // Per-layer Bitboard Data
    uint32_t layerMask[32];
    uint16_t palettes[32][32];

    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        for (int normalDir = 1; normalDir >= -1; normalDir -= 2) {
            const uint8_t faceID = static_cast<uint8_t>(d * 2 + (normalDir > 0 ? 0 : 1));

            for (int layer = 0; layer < gridSize; ++layer) {
                
                // Clear Bitboard
                for (int m = 0; m < gridSize; ++m) {
                    layerMask[m] = 0;
                }

                // 1. Generate Bitmask and extract palettes
                for (int j = 0; j < gridSize; ++j) {
                    for (int i = 0; i < gridSize; ++i) {
                        int pos[3];
                        pos[d] = layer * step;
                        pos[u] = i     * step;
                        pos[v] = j     * step;

                        const VoxelData& vox = m_voxels[idx(pos[0], pos[1], pos[2])];
                        if (!vox.isSolid()) continue;

                        int npos[3] = { pos[0], pos[1], pos[2] };
                        npos[d] += normalDir * step; // Base origin of the neighbor LOD block!
                        
                        // Exact single-voxel sampling for Culling.
                        // Essential to match the single-voxel sampling for Block Presence!
                        bool isNeighborSolid = volumeCache[cacheIdx(npos[0], npos[1], npos[2])].isSolid();

                        // FORCE GENERATE FACES ON CHUNK BOUNDARIES FOR SKIRTS!
                        // If we are at the edge of the chunk (layer == 0 && normalDir == -1 OR layer == gridSize-1 && normalDir == 1)
                        // AND we are generating LOD > 0, we MUST generate the face to pull it down as a skirt!
                        // Otherwise, the Culling will remove the face because the neighbor is solid, leaving a gap!
                        if (lod > 0 && (d == 0 || d == 2)) {
                            bool isBoundary = ((layer == 0 && normalDir == -1) || (layer == gridSize - 1 && normalDir == 1));
                            if (isBoundary) {
                                isNeighborSolid = false; // Force face generation
                            }
                        }

                        if (isNeighborSolid) continue;

                        layerMask[j] |= (1u << i);
                        palettes[j][i] = vox.getPaletteIndex();
                    }
                }

                // 2. Bitwise Greedy Meshing
                for (int j = 0; j < gridSize; ++j) {
                    while (layerMask[j] != 0) {
                        int i = std::countr_zero(layerMask[j]);
                        uint16_t p = palettes[j][i];
                        
                        int W = 1;
                        uint32_t rowMask = (1u << i);
                        while (i + W < gridSize && (layerMask[j] & (1u << (i + W))) && palettes[j][i + W] == p) {
                            rowMask |= (1u << (i + W));
                            W++;
                        }
                        
                        int H = 1;
                        while (j + H < gridSize) {
                            if ((layerMask[j + H] & rowMask) != rowMask) break;
                            
                            bool match = true;
                            for (int k = 0; k < W; ++k) {
                                if (palettes[j + H][i + k] != p) {
                                    match = false;
                                    break;
                                }
                            }
                            if (!match) break;
                            H++;
                        }
                        
                        // Delayed AO Calculation (Compute ONLY for the 4 corners of the merged face!)
                        int aoPos0[3], aoPos1[3], aoPos2[3], aoPos3[3];
                        aoPos0[d] = aoPos1[d] = aoPos2[d] = aoPos3[d] = layer * step;
                        
                        aoPos0[u] = i * step;             aoPos0[v] = j * step;
                        aoPos1[u] = (i + W - 1) * step;   aoPos1[v] = j * step;
                        aoPos2[u] = (i + W - 1) * step;   aoPos2[v] = (j + H - 1) * step;
                        aoPos3[u] = i * step;             aoPos3[v] = (j + H - 1) * step;
                        
                        uint8_t ao0 = sampleAO(volumeCache, aoPos0, d, -1, -1, normalDir);
                        uint8_t ao1 = sampleAO(volumeCache, aoPos1, d, +1, -1, normalDir);
                        uint8_t ao2 = sampleAO(volumeCache, aoPos2, d, +1, +1, normalDir);
                        uint8_t ao3 = sampleAO(volumeCache, aoPos3, d, -1, +1, normalDir);
                        
                        // Emit Quad Output
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
                        
                        // Skirts logic for LOD > 0 to hide seams
                        if (lod > 0) {
                            if (d == 0 || d == 2) {
                                bool isBoundary = ((layer == 0 && normalDir == -1) || (layer == gridSize - 1 && normalDir == 1));
                                if (isBoundary) {
                                    int drop = step * 2;
                                    // Identify bottom vertices (which have the smallest Y)
                                    int minY = std::min({corners[0][1], corners[1][1], corners[2][1], corners[3][1]});
                                    for (int c = 0; c < 4; ++c) {
                                        if (corners[c][1] == minY) {
                                            corners[c][1] -= drop;
                                        }
                                    }
                                }
                            }
                        }
                        
                        emitQuad(mesh, corners, faceID, p, ao0, ao1, ao2, ao3, normalDir);
                        
                        uint32_t clearMask = ~rowMask;
                        for (int h = 0; h < H; ++h) {
                            layerMask[j + h] &= clearMask;
                        }
                    }
                }
            }
        }
    }

    return mesh;
}

} // namespace world
