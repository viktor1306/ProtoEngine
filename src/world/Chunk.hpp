#pragma once

#include "VoxelData.hpp"
#include "gfx/resources/Mesh.hpp"
#include <vector>
#include <array>
#include <cstdint>
#include <atomic>

class FastNoiseLite;

namespace world {

enum class ChunkState : uint8_t {
    // Placeholder chunk exists in storage but voxel payload is not ready yet.
    UNGENERATED = 0,
    // A worker task owns fillTerrain() for this chunk right now.
    GENERATING  = 1,
    // Voxel payload is ready for meshing / neighbour queries.
    READY       = 2
};

constexpr int CHUNK_SIZE = 32;

// CPU-side voxel mesh data (uses compressed VoxelVertex — 8 bytes each)
struct VoxelMeshData {
    std::vector<VoxelVertex> vertices;
    std::vector<uint32_t>    indices;
    bool empty() const { return vertices.empty(); }
};

struct TerrainConfig {
    // --- Core noise ---
    int   seed          = 42;
    int   baseHeight    = 64;    // mean land height in blocks
    float amplitude     = 60.0f; // max height variation above baseHeight
    int   octaves       = 6;
    float frequency     = 0.015f; // base noise frequency

    // --- World Scale ---
    // worldScale > 1 → wider/larger features (lower effective frequency)
    // worldScale < 1 → tighter/smaller features (higher effective frequency)
    float worldScale    = 1.0f;

    // --- Island mask ---
    bool  islandMode      = true;
    float islandFalloff   = 0.60f; // fraction of worldRadius where falloff begins
    float islandEdgeNoise = 0.28f; // raggedness of the coastline (0=circle, 1=very jagged)
    int   worldRadiusBlks = 320;   // world half-width in blocks (set by generateWorld)

    // --- Water / Sea level ---
    int   seaLevel    = 52; // Y below which open voxels become WATER
    int   sandMargin  = 5;  // blocks above seaLevel that become SAND

    // --- Snow caps ---
    int   snowHeight  = 115; // Y above which the surface becomes SNOW

    // --- Mountain / Erosion ---
    // mountainStrength: 0=all flat plains, 1=dramatic sharp peaks
    float mountainStrength    = 0.70f;
    // stoneErosionThresh: erosion above this → bare rock cliff (no grass/dirt)
    // Lower = more bare stone, Higher = more grass coverage on slopes
    float stoneErosionThresh  = 0.72f;

    // --- Desert / Moisture ---
    // desertMoistureThresh: moisture below this → sand/desert biome
    // Lower (more negative) = less desert, Higher (toward 0) = more desert
    float desertMoistureThresh = -0.15f;

    // --- Rivers ---
    int   riverDepth  = 18;   // blocks below seaLevel that river trenches carve
    float riverWidth  = 0.10f; // ridged-noise threshold: lower = narrower rivers
};

class Chunk {
public:
    // chunkCoord: grid position (multiply by CHUNK_SIZE to get world offset)
    explicit Chunk(int cx = 0, int cy = 0, int cz = 0);
    
    void reset(int cx, int cy, int cz) {
        m_cx = cx; m_cy = cy; m_cz = cz;
        m_isDirty = true;
        m_state.store(ChunkState::UNGENERATED, std::memory_order_release);
    }

    // ---- Voxel access -------------------------------------------------------
    void      setVoxel(int x, int y, int z, VoxelData v);
    VoxelData getVoxel(int x, int y, int z) const;

    // ---- Fill helpers -------------------------------------------------------
    void fill(VoxelData v);
    void fillTerrain(const TerrainConfig& config, FastNoiseLite* noise = nullptr);   // heightmap-based terrain
    void fillRandom(int seed = 0);    // random solid/air for testing

    // ---- Mesh generation ----------------------------------------------------
    // Hidden Face Culling — only emit faces adjacent to AIR.
    // neighbors[6]: adjacent chunks in order +X,-X,+Y,-Y,+Z,-Z.
    // Pass nullptr for a neighbour to treat that boundary as AIR.
    // Coordinates in VoxelVertex are LOCAL (0-31) — chunk offset is applied
    // in the vertex shader via push constants (chunkOffset).
    //
    // lod: Level of Detail (0=full, 1=half, 2=quarter resolution)
    //   step = 1 << lod  (1, 2, or 4 voxels per super-voxel)
    //   LOD 0: every voxel, full Greedy Meshing
    //   LOD 1: 2×2×2 super-voxels, ~4× fewer vertices
    //   LOD 2: 4×4×4 super-voxels, ~16× fewer vertices
    VoxelMeshData generateMesh(const std::array<const Chunk*, 6>& neighbors = {},
                               const std::array<int, 6>& neighborLODs = {},
                               int lod = 0) const;

    // ---- State --------------------------------------------------------------
    bool isDirty()  const { return m_isDirty; }
    void markDirty()      { m_isDirty = true; }
    void markClean()      { m_isDirty = false; }

    // Grid coordinate of this chunk
    int getCX() const { return m_cx; }
    int getCY() const { return m_cy; }
    int getCZ() const { return m_cz; }

    // Storage lifecycle state. This tracks voxel readiness only; GPU mesh state lives in ChunkRenderer.
    std::atomic<ChunkState> m_state{ChunkState::READY};

    // Tracks player edits to avoid destroying chunk data during Tier-4 stream unloads
    std::atomic<bool> m_isModified{false};

    // Current render lifecycle marker assigned by ChunkManager / ChunkRenderer.
    // Values: -1 (voxel data ready but no GPU mesh assigned yet), -2 (mesh evicted, voxels kept), or 0,1,2...
    std::atomic<int> m_currentLOD{-1};

    // World-space offset of this chunk's (0,0,0) corner (in block units)
    float getWorldOffsetX() const { return static_cast<float>(m_cx * CHUNK_SIZE); }
    float getWorldOffsetY() const { return static_cast<float>(m_cy * CHUNK_SIZE); }
    float getWorldOffsetZ() const { return static_cast<float>(m_cz * CHUNK_SIZE); }

    // Returns true if the voxel at local (x,y,z) is non-solid (AIR).
    // Out-of-bounds coords query the appropriate neighbour chunk.
    // If neighbour is nullptr, treat as AIR (emit face at world boundary).
    // Public: needed by sampleAO() free function in Chunk.cpp.
    bool isAirAt(int x, int y, int z,
                 const std::array<const Chunk*, 6>& neighbors) const;

    // Compute simple AO value (0-3) for a face vertex.
    // Public: needed by sampleAO() free function in Chunk.cpp.
    static uint8_t computeAO(bool side1, bool side2, bool corner);

private:
    VoxelData m_voxels[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE]{};
    int  m_cx, m_cy, m_cz;
    bool m_isDirty = true;

    static int idx(int x, int y, int z) {
        return x + y * CHUNK_SIZE + z * CHUNK_SIZE * CHUNK_SIZE;
    }
};

} // namespace world
