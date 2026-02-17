#pragma once

#include "BlockType.hpp"
#include "gfx/resources/Mesh.hpp"
#include <vector>
#include <cstdint>

namespace world {

constexpr int CHUNK_SIZE = 16;

// CPU-side mesh data ready to be uploaded via GeometryManager::uploadMesh().
// Chunk never touches Vulkan directly.
struct MeshData {
    std::vector<gfx::Vertex>   vertices;
    std::vector<uint32_t>      indices;

    bool empty() const { return vertices.empty(); }
};

class Chunk {
public:
    // worldPos: position of the chunk's (0,0,0) corner in world space (block units)
    explicit Chunk(core::math::Vec3 worldPos = {0.0f, 0.0f, 0.0f});

    // Block access — bounds-checked in debug, unchecked in release
    void    setBlock(int x, int y, int z, BlockID id);
    BlockID getBlock(int x, int y, int z) const;

    // Fill entire chunk with one block type
    void fill(BlockID id);

    // Fill with a simple terrain: stone below groundY, grass at groundY, air above
    void fillTerrain(int groundY = 8);

    // Fill with random blocks (stone/grass/air) — for testing Culled Meshing
    void fillRandom();

    // Culled Meshing: only emit faces adjacent to AIR or chunk boundary.
    // Returns CPU vertex/index data ready for GeometryManager::uploadMesh().
    MeshData generateMesh() const;

    core::math::Vec3 getWorldPos() const { return m_worldPos; }

private:
    // Flat 3D array: index = x + y*CHUNK_SIZE + z*CHUNK_SIZE*CHUNK_SIZE
    BlockID m_blocks[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE]{};
    core::math::Vec3 m_worldPos;

    // Returns true if the block at (x,y,z) is transparent/non-solid.
    // Out-of-bounds coords are treated as AIR (chunk boundary → emit face).
    bool isAirAt(int x, int y, int z) const;

    static int idx(int x, int y, int z) {
        return x + y * CHUNK_SIZE + z * CHUNK_SIZE * CHUNK_SIZE;
    }
};

} // namespace world
