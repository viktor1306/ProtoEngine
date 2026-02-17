#include "Chunk.hpp"
#include <cstdlib>  // rand()
#include <cmath>    // std::abs

namespace world {

// ---------------------------------------------------------------------------
// Face definitions: 6 faces, each with 4 vertices (quad) and a normal.
// Vertex positions are offsets from the block's (x,y,z) corner.
// Winding order: counter-clockwise when viewed from outside (matches main pipeline).
// ---------------------------------------------------------------------------
struct FaceDef {
    // 4 corner offsets (relative to block origin)
    float verts[4][3];
    // Face normal
    float normal[3];
    // Neighbour offset to check for occlusion
    int nx, ny, nz;
};

static constexpr FaceDef k_faces[6] = {
    // +X (Right)
    {{{1,0,0},{1,1,0},{1,1,1},{1,0,1}}, { 1, 0, 0},  1, 0, 0},
    // -X (Left)
    {{{0,0,1},{0,1,1},{0,1,0},{0,0,0}}, {-1, 0, 0}, -1, 0, 0},
    // +Y (Top)
    {{{0,1,0},{0,1,1},{1,1,1},{1,1,0}}, { 0, 1, 0},  0, 1, 0},
    // -Y (Bottom)
    {{{0,0,1},{0,0,0},{1,0,0},{1,0,1}}, { 0,-1, 0},  0,-1, 0},
    // +Z (Front)
    {{{0,0,1},{1,0,1},{1,1,1},{0,1,1}}, { 0, 0, 1},  0, 0, 1},
    // -Z (Back)
    {{{1,0,0},{0,0,0},{0,1,0},{1,1,0}}, { 0, 0,-1},  0, 0,-1},
};

// ---------------------------------------------------------------------------
Chunk::Chunk(core::math::Vec3 worldPos)
    : m_worldPos(worldPos)
{
    // Default: all AIR
    std::fill(std::begin(m_blocks), std::end(m_blocks), BlockID{0});
}

void Chunk::setBlock(int x, int y, int z, BlockID id) {
    m_blocks[idx(x, y, z)] = id;
}

BlockID Chunk::getBlock(int x, int y, int z) const {
    return m_blocks[idx(x, y, z)];
}

void Chunk::fill(BlockID id) {
    std::fill(std::begin(m_blocks), std::end(m_blocks), id);
}

void Chunk::fillTerrain(int groundY) {
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                BlockID id = 0; // AIR
                if (y < groundY - 1)
                    id = 1; // STONE
                else if (y == groundY - 1)
                    id = 3; // DIRT
                else if (y == groundY)
                    id = 2; // GRASS
                // above groundY → AIR
                setBlock(x, y, z, id);
            }
        }
    }
}

void Chunk::fillRandom() {
    static const BlockID palette[] = {0, 0, 0, 1, 1, 2}; // weighted: more air
    for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE; ++i) {
        m_blocks[i] = palette[rand() % 6];
    }
}

bool Chunk::isAirAt(int x, int y, int z) const {
    // Out-of-bounds → treat as AIR (emit face at chunk boundary)
    if (x < 0 || x >= CHUNK_SIZE ||
        y < 0 || y >= CHUNK_SIZE ||
        z < 0 || z >= CHUNK_SIZE)
        return true;
    return !BlockRegistry::isSolid(m_blocks[idx(x, y, z)]);
}

MeshData Chunk::generateMesh() const {
    MeshData data;
    data.vertices.reserve(4096);
    data.indices.reserve(6144);

    uint32_t vertBase = 0;

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                BlockID bid = m_blocks[idx(x, y, z)];
                if (!BlockRegistry::isSolid(bid))
                    continue; // skip AIR / transparent blocks

                const BlockInfo& info = BlockRegistry::get(bid);
                // Vertex color from block definition (RGB from Vec4)
                float r = info.color.x;
                float g = info.color.y;
                float b = info.color.z;

                // World-space origin of this block
                float wx = m_worldPos.x + static_cast<float>(x);
                float wy = m_worldPos.y + static_cast<float>(y);
                float wz = m_worldPos.z + static_cast<float>(z);

                for (const auto& face : k_faces) {
                    // Only emit face if neighbour is AIR
                    if (!isAirAt(x + face.nx, y + face.ny, z + face.nz))
                        continue;

                    // 4 vertices for this quad
                    for (int v = 0; v < 4; ++v) {
                        gfx::Vertex vert{};
                        vert.position = {wx + face.verts[v][0],
                                         wy + face.verts[v][1],
                                         wz + face.verts[v][2]};
                        vert.normal   = {face.normal[0], face.normal[1], face.normal[2]};
                        vert.color    = {r, g, b};
                        vert.uv       = {0.0f, 0.0f}; // texture atlas later
                        data.vertices.push_back(vert);
                    }

                    // 2 triangles (CCW): 0-1-2, 0-2-3
                    data.indices.push_back(vertBase + 0);
                    data.indices.push_back(vertBase + 1);
                    data.indices.push_back(vertBase + 2);
                    data.indices.push_back(vertBase + 0);
                    data.indices.push_back(vertBase + 2);
                    data.indices.push_back(vertBase + 3);
                    vertBase += 4;
                }
            }
        }
    }

    return data;
}

} // namespace world
