#pragma once

#include "VoxelData.hpp"
#include "gfx/resources/Mesh.hpp"
#include <vector>
#include <array>
#include <cstdint>

namespace world {

constexpr int CHUNK_SIZE = 32;

// CPU-side voxel mesh data (uses compressed VoxelVertex — 8 bytes each)
struct VoxelMeshData {
    std::vector<VoxelVertex> vertices;
    std::vector<uint32_t>    indices;
    bool empty() const { return vertices.empty(); }
};

class Chunk {
public:
    // chunkCoord: grid position (multiply by CHUNK_SIZE to get world offset)
    explicit Chunk(int cx = 0, int cy = 0, int cz = 0);

    // ---- Voxel access -------------------------------------------------------
    void      setVoxel(int x, int y, int z, VoxelData v);
    VoxelData getVoxel(int x, int y, int z) const;

    // ---- Fill helpers -------------------------------------------------------
    void fill(VoxelData v);
    void fillTerrain(int seed = 0);   // heightmap-based terrain
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
                               int lod = 0) const;

    // ---- State --------------------------------------------------------------
    bool isDirty()  const { return m_isDirty; }
    void markDirty()      { m_isDirty = true; }
    void markClean()      { m_isDirty = false; }

    // Grid coordinate of this chunk
    int getCX() const { return m_cx; }
    int getCY() const { return m_cy; }
    int getCZ() const { return m_cz; }

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
