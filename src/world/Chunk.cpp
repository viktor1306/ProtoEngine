#include "world/Chunk.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include "../vendor/FastNoiseLite.h"

namespace world {

// ---------------------------------------------------------------------------
// Chunk
// ---------------------------------------------------------------------------
Chunk::Chunk(int cx, int cy, int cz) : m_cx(cx), m_cy(cy), m_cz(cz) {}

void Chunk::setVoxel(int x, int y, int z, VoxelData v) {
    m_voxels[idx(x, y, z)] = v;
    m_isDirty = true;
    m_isModified = true; // Mark as modified by player to save in RAM cache
}

VoxelData Chunk::getVoxel(int x, int y, int z) const {
    return m_voxels[idx(x, y, z)];
}

void Chunk::fill(VoxelData v) {
    for (auto& vox : m_voxels) vox = v;
    m_isDirty = true;
}

// ---------------------------------------------------------------------------
// Island Generation Helpers
// ---------------------------------------------------------------------------

// Smooth hermite blend (C1 continuity): 0 at t=0, 1 at t=1
static inline float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Returns [0,1]: 1.0 = centre of island, 0.0 = ocean edge.
// Uses a low-frequency noise offset so the coastline is organic/ragged.
static float computeIslandMask(float wx, float wz,
                                const TerrainConfig& cfg,
                                FastNoiseLite& maskNoise)
{
    const float rBlks = static_cast<float>(cfg.worldRadiusBlks);
    // Normalised distance from world centre: 0 at centre, 1 at edge
    float nx   = wx / rBlks;
    float nz   = wz / rBlks;
    float dist = std::sqrt(nx * nx + nz * nz);

    // Low-frequency warp makes the shoreline non-circular
    float warp = maskNoise.GetNoise(wx, wz);  // maskNoise freq ≈ 0.003
    float raggedDist = dist - warp * cfg.islandEdgeNoise;

    // Smooth falloff: full land inside falloff, ocean beyond 1.1
    float t = (raggedDist - cfg.islandFalloff) / (1.1f - cfg.islandFalloff);
    return 1.0f - smoothstep01(t);
}

// (getBaseHeight removed — height formula is now inlined in fillTerrain
//  as a blend of plainH and mountH, controlled by erosion noise.)

// ---------------------------------------------------------------------------
// fillTerrain — island + biomes (erosion / rivers / moisture) + water
// ---------------------------------------------------------------------------
void Chunk::fillTerrain(const TerrainConfig& config, FastNoiseLite* /*extNoise*/) {

    // ---- Voxel constants ---------------------------------------------------
    const VoxelData vStone = VoxelData::make(1, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData vGrass = VoxelData::make(2, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData vDirt  = VoxelData::make(3, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData vSand  = VoxelData::make(4, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData vSnow  = VoxelData::make(5, 255, 0, VOXEL_FLAG_SOLID);
    const VoxelData vWater = VoxelData::make(6, 255, 0, VOXEL_FLAG_SOLID | VOXEL_FLAG_LIQUID);

    // ---- Noise layer 1: Base terrain (FBm) ---------------------------------
    FastNoiseLite terrainNoise;
    terrainNoise.SetSeed(config.seed);
    terrainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    terrainNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    terrainNoise.SetFractalOctaves(config.octaves);
    terrainNoise.SetFrequency(config.frequency);

    // ---- Noise layer 2: Island mask warp (very low frequency) ---------------
    FastNoiseLite maskNoise;
    maskNoise.SetSeed(config.seed + 1);
    maskNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    maskNoise.SetFrequency(0.003f);

    // ---- Noise layer 3: Erosion — flat plains vs sharp mountains (medium freq) --
    FastNoiseLite erosionNoise;
    erosionNoise.SetSeed(config.seed + 2);
    erosionNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    erosionNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    erosionNoise.SetFractalOctaves(4);
    erosionNoise.SetFrequency(0.006f); // large-scale mountain/plain zones

    // ---- Noise layer 4: River carving (ridged — trench where |n| < riverWidth) --
    FastNoiseLite riverNoise;
    riverNoise.SetSeed(config.seed + 3);
    riverNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    riverNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    riverNoise.SetFractalOctaves(3);
    riverNoise.SetFrequency(0.004f);

    // ---- Noise layer 5: Moisture — desert (dry) vs forest/plains (wet) ------
    FastNoiseLite moistureNoise;
    moistureNoise.SetSeed(config.seed + 4);
    moistureNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    moistureNoise.SetFrequency(0.005f);

    // ---- World-space origins of this chunk ---------------------------------
    const int worldBaseX = m_cx * CHUNK_SIZE;
    const int worldBaseY = m_cy * CHUNK_SIZE;
    const int worldBaseZ = m_cz * CHUNK_SIZE;

    // ---- Sample all noise layers at low-res grid (STEP=4, SAMPLES=9) -------
    constexpr int STEP    = 4;
    constexpr int SAMPLES = CHUNK_SIZE / STEP + 1; // 9×9 sample grid

    float sH[SAMPLES][SAMPLES]; // final terrain height
    float sE[SAMPLES][SAMPLES]; // erosion [0,1]:  0=plain, 1=mountain
    float sR[SAMPLES][SAMPLES]; // river carve [0,1]: 1=deepest trench
    float sM[SAMPLES][SAMPLES]; // moisture [-1,1]: -1=desert, +1=tropical

    const float riverFloor = (float)(config.seaLevel - config.riverDepth);
    const float rWidth     = std::max(config.riverWidth, 0.005f);
    const float oceanFloor = (float)(config.seaLevel - 20);

    for (int sz = 0; sz < SAMPLES; ++sz) {
        for (int sx = 0; sx < SAMPLES; ++sx) {
            const float wx = (float)(worldBaseX + sx * STEP);
            const float wz = (float)(worldBaseZ + sz * STEP);
            const float qx = wx / config.worldScale; // scaled coords for terrain
            const float qz = wz / config.worldScale;

            // Island mask [0,1] — 1=island interior, 0=open ocean
            const float mask   = computeIslandMask(wx, wz, config, maskNoise);

            // Base terrain noise in [-1, 1]
            const float terrN  = terrainNoise.GetNoise(qx, qz);

            // Erosion [0,1]: capped at island edges so coast is always flat
            const float rawE   = (erosionNoise.GetNoise(qx, qz) + 1.0f) * 0.5f;
            const float erode  = rawE * smoothstep01(mask * 1.6f);

            // Moisture [-1, 1]
            const float moist  = moistureNoise.GetNoise(wx, wz);

            // --- Height formula ---
            // Plains contribution: gentle hills (40% amplitude)
            const float plainH = (float)config.baseHeight + terrN * config.amplitude * 0.40f;

            // Mountain contribution: squared noise → sharp peaks
            // absN²*2.2 reaches ~2.2 at |terrN|=1; subtract 0.25 to keep low
            // areas from also rising
            const float absN   = std::abs(terrN);
            const float mountH = (float)config.baseHeight
                                 + (absN * absN * 2.2f - 0.25f)
                                 * config.amplitude * config.mountainStrength;

            // Blend plains↔mountains by erosion
            const float blendH = plainH + (mountH - plainH) * erode;

            // Apply island mask → lerp toward ocean floor at edges
            float finalH = oceanFloor + (blendH - oceanFloor) * mask;

            // --- River carving ---
            // Ridged: |riverNoise| is small near river centrelines
            const float rn     = std::abs(riverNoise.GetNoise(qx, qz));
            float rAmt         = std::max(0.0f, 1.0f - rn / rWidth); // 0→1
            rAmt               = rAmt * rAmt; // sharpen profile
            // Only carve rivers where island exists (not in city ocean zone)
            const float coastBlend = std::clamp((mask - 0.25f) / 0.35f, 0.0f, 1.0f);
            rAmt                  *= coastBlend;
            // Pull height toward river floor
            finalH = finalH + (riverFloor - finalH) * rAmt;

            sH[sz][sx] = finalH;
            sE[sz][sx] = erode;
            sR[sz][sx] = rAmt;
            sM[sz][sx] = moist;
        }
    }

    // ---- Bilinear interpolation into full-res chunk maps -------------------
    float heightmap[CHUNK_SIZE][CHUNK_SIZE];
    float erodemap [CHUNK_SIZE][CHUNK_SIZE];
    float rivermap [CHUNK_SIZE][CHUNK_SIZE];
    float moistmap [CHUNK_SIZE][CHUNK_SIZE];

    for (int sz = 0; sz < SAMPLES - 1; ++sz) {
        for (int sx = 0; sx < SAMPLES - 1; ++sx) {
            // Cache the 4 corners for each field once
            const float H00=sH[sz][sx], H10=sH[sz][sx+1], H01=sH[sz+1][sx], H11=sH[sz+1][sx+1];
            const float E00=sE[sz][sx], E10=sE[sz][sx+1], E01=sE[sz+1][sx], E11=sE[sz+1][sx+1];
            const float R00=sR[sz][sx], R10=sR[sz][sx+1], R01=sR[sz+1][sx], R11=sR[sz+1][sx+1];
            const float M00=sM[sz][sx], M10=sM[sz][sx+1], M01=sM[sz+1][sx], M11=sM[sz+1][sx+1];

            for (int dz = 0; dz < STEP; ++dz) {
                const float tz = (float)dz / STEP;
                // Lerp along Z for each field
                const float hH0 = H00 + (H01-H00)*tz,  hH1 = H10 + (H11-H10)*tz;
                const float hE0 = E00 + (E01-E00)*tz,  hE1 = E10 + (E11-E10)*tz;
                const float hR0 = R00 + (R01-R00)*tz,  hR1 = R10 + (R11-R10)*tz;
                const float hM0 = M00 + (M01-M00)*tz,  hM1 = M10 + (M11-M10)*tz;

                for (int dx = 0; dx < STEP; ++dx) {
                    const int gx = sx * STEP + dx;
                    const int gz = sz * STEP + dz;
                    if (gx >= CHUNK_SIZE || gz >= CHUNK_SIZE) continue;
                    const float tx = (float)dx / STEP;
                    heightmap[gz][gx] = hH0 + (hH1-hH0)*tx;
                    erodemap [gz][gx] = hE0 + (hE1-hE0)*tx;
                    rivermap [gz][gx] = hR0 + (hR1-hR0)*tx;
                    moistmap [gz][gx] = hM0 + (hM1-hM0)*tx;
                }
            }
        }
    }

    // ---- Fill voxels -------------------------------------------------------
    std::ranges::fill(m_voxels, VOXEL_AIR);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            const int   terrainH = (int)heightmap[z][x];
            const float erode01  = erodemap[z][x];    // [0,1]: 0=flat plain, 1=sharp mountain
            [[maybe_unused]]
            const float rAmt     = rivermap[z][x];    // river carve strength
            const float moist    = moistmap[z][x];    // [-1,1]: -1=dry, +1=wet

            // ---------------------------------------------------------------
            // Biome flags  (in priority order)
            // ---------------------------------------------------------------

            // Rocky cliff: very high erosion → bare stone face, grass cannot grip
            const bool isRockyCliff = (erode01 > config.stoneErosionThresh);

            // Desert: dry moisture AND not too high (low-elevation sandy zone)
            const bool isDesert = (moist < config.desertMoistureThresh)
                               && (terrainH < config.seaLevel + 50);

            // Beach: within sandMargin blocks of sea level
            const bool isBeach = (terrainH <= config.seaLevel + config.sandMargin);

            // Snow cap: surface is above snowHeight
            const bool isSnowCap = (terrainH > config.snowHeight);

            // ---------------------------------------------------------------
            // 4-level column layering:
            //
            //  Plain / grassy mountain          Rocky cliff      Desert / Beach
            //  ─────────────────────────        ─────────────    ──────────────
            //  depth 1 : GRASS (or SNOW cap)    STONE            SAND
            //  depth 2-4: DIRT  (or STONE cap)  STONE            SAND
            //  depth 5+: STONE                  STONE            STONE
            //
            //  Snow cap override (terrainH > snowHeight):
            //    depth 1   → SNOW
            //    depth 2-3 → STONE  (rock just below snowfield, no dirt)
            //    depth 4+  → STONE
            // ---------------------------------------------------------------

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                const int wy    = worldBaseY + y;
                VoxelData v     = VOXEL_AIR;

                if (wy < terrainH) {
                    const int depth = terrainH - wy;  // 1=surface, 2=one below, …

                    if (isSnowCap) {
                        // Snow cap: SNOW on top, immediate stone underneath
                        if      (depth == 1) v = vSnow;
                        else                 v = vStone;

                    } else if (isRockyCliff) {
                        // Bare cliff: all stone
                        v = vStone;

                    } else if (isDesert || isBeach) {
                        // Sandy biome: sand surface + sand subsurface, then stone
                        if   (depth <= 4) v = vSand;
                        else              v = vStone;

                    } else {
                        // Normal grassy land (plains AND grassy mountain slopes):
                        // GRASS → DIRT (4 blocks) → STONE
                        if      (depth == 1) v = vGrass;
                        else if (depth <= 5) v = vDirt;
                        else                 v = vStone;
                    }

                } else if (wy < config.seaLevel) {
                    v = vWater;
                }

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
                        const std::array<int, 3>& pos, int d, int du, int dv, int normalDir)
{
    const int u = (d + 1) % 3;
    const int v = (d + 2) % 3;

    std::array<int, 3> base = pos;
    base[d] += (normalDir > 0) ? 1 : -1;

    std::array<int, 3> s1 = base; s1[u] += du;
    std::array<int, 3> s2 = base; s2[v] += dv;
    std::array<int, 3> sc = base; sc[u] += du; sc[v] += dv;

    bool b1 = cache[cacheIdx(s1[0], s1[1], s1[2])].isSolid();
    bool b2 = cache[cacheIdx(s2[0], s2[1], s2[2])].isSolid();
    bool bc = cache[cacheIdx(sc[0], sc[1], sc[2])].isSolid();

    return Chunk::computeAO(b1, b2, bc);
}

static void emitQuad(VoxelMeshData& mesh,
                     std::span<const std::array<int, 3>, 4> corners,
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
        const auto& co = corners[vOrder[c]];
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
                                  const std::array<int, 6>& neighborLODs,
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
                    if (nb->m_state.load(std::memory_order_acquire) == ChunkState::READY) {
                        lx = std::clamp(lx, 0, CHUNK_SIZE - 1);
                        ly = std::clamp(ly, 0, CHUNK_SIZE - 1);
                        lz = std::clamp(lz, 0, CHUNK_SIZE - 1);
                        volumeCache[cacheIdx(x, y, z)] = nb->getVoxel(lx, ly, lz);
                    } else {
                        // If neighbor is UNGENERATED/GENERATING, assume it's SOLID.
                        // This prevents creating "walls" on the boundary before the
                        // terrain below is actually loaded by progressive generation.
                        volumeCache[cacheIdx(x, y, z)] = VoxelData::make(1, 255, 0, VOXEL_FLAG_SOLID);
                    }
                }
            }
        }
    }

    // Per-layer Bitboard Data
    static_assert(CHUNK_SIZE <= 32, "Greedy meshing bitmask overflow: CHUNK_SIZE > 32 requires 64-bit masks");
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
                        std::array<int, 3> pos;
                        pos[d] = layer * step;
                        pos[u] = i     * step;
                        pos[v] = j     * step;

                        const VoxelData& vox = m_voxels[idx(pos[0], pos[1], pos[2])];
                        if (!vox.isSolid()) continue;

                        std::array<int, 3> npos = pos;
                        npos[d] += normalDir * step; // Base origin of the neighbor LOD block!

                        // ------------------------------------------------------------------
                        // SMART SKIRTS: Інтеграція в Bitwise Greedy Meshing
                        // Якщо сусід відсутній (край світу) або має інший рівень деталізації (LOD):
                        // Ми примусово вважаємо цю межу ПОВІТРЯМ (AIR -> isNeighborSolid = false).
                        // Завдяки цьому, код нижче запише `1` у layerMask для КОЖНОГО вокселя обличчя (напр. 32x32), 
                        // і Greedy Meshing автоматично об'єднає всю цю площину в ОДИН великий Quad!
                        // ------------------------------------------------------------------
                        bool isNeighborSolid = false;
                        if (npos[d] >= 0 && npos[d] < CHUNK_SIZE) {
                            // Internal voxel check
                            isNeighborSolid = m_voxels[idx(npos[0], npos[1], npos[2])].isSolid();
                        } else {
                            // Boundary voxel check
                            int neighborIdx = -1;
                            int lx = npos[0], ly = npos[1], lz = npos[2];
                            
                            if (d == 0) {
                                neighborIdx = (normalDir > 0) ? 0 : 1;
                                lx = (normalDir > 0) ? 0 : (CHUNK_SIZE - step);
                            } else if (d == 1) {
                                neighborIdx = (normalDir > 0) ? 2 : 3;
                                ly = (normalDir > 0) ? 0 : (CHUNK_SIZE - step);
                            } else {
                                neighborIdx = (normalDir > 0) ? 4 : 5;
                                lz = (normalDir > 0) ? 0 : (CHUNK_SIZE - step);
                            }
                            
                            const Chunk* nb = neighbors[neighborIdx];
                            if (!nb || neighborLODs[neighborIdx] != lod) {
                                // Спідниця: Edge of world OR LOD Boundary -> Повітря (щоб генерувався єдиний Quad)
                                isNeighborSolid = false; 
                            } else if (nb->m_state.load(std::memory_order_acquire) == ChunkState::READY) {
                                // Same LOD: exact fast O(1) local voxel lookup
                                isNeighborSolid = nb->m_voxels[idx(lx, ly, lz)].isSolid();
                            } else {
                                // Підземні чанки в процесі генерації (Sparse Storage)
                                isNeighborSolid = true;
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
                        std::array<int, 3> aoPos0, aoPos1, aoPos2, aoPos3;
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
                        
                        std::array<std::array<int, 3>, 4> corners;
                        corners[0][d]=faceLayer; corners[0][u]=vi;    corners[0][v]=vj;
                        corners[1][d]=faceLayer; corners[1][u]=vi+vW; corners[1][v]=vj;
                        corners[2][d]=faceLayer; corners[2][u]=vi+vW; corners[2][v]=vj+vH;
                        corners[3][d]=faceLayer; corners[3][u]=vi;    corners[3][v]=vj+vH;
                        
                        
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
