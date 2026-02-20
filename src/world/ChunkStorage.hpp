#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include "world/Chunk.hpp"

namespace world {

// Use the same hashing as before for backward compatibility or sparse keys
struct IVec3Key {
    int x, y, z;
    bool operator==(const IVec3Key& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct IVec3Hash {
    std::size_t operator()(const IVec3Key& k) const noexcept {
        auto hx = std::hash<int>{}(k.x);
        auto hy = std::hash<int>{}(k.y);
        auto hz = std::hash<int>{}(k.z);
        return hx ^ (hy << 1) ^ (hz << 2);
    }
};

class ChunkRenderer; // Forward declaration

class ChunkStorage {
public:
    void generateWorld(int radiusX, int radiusZ, int seed = 42);
    void clear();
    void removeChunk(int cx, int cy, int cz);
    
    void createChunkIfMissing(int cx, int cy, int cz, int seed, ChunkRenderer& renderer);

    VoxelData getVoxel(int wx, int wy, int wz) const;
    void      setVoxel(int wx, int wy, int wz, VoxelData v);

    const Chunk* getChunk(int cx, int cy, int cz) const;
    Chunk*       getChunk(int cx, int cy, int cz);

    const std::vector<std::unique_ptr<Chunk>>& getChunks() const { return m_activeChunks; }
    std::vector<std::unique_ptr<Chunk>>&       getChunks()       { return m_activeChunks; }

private:
    std::vector<std::unique_ptr<Chunk>> m_activeChunks;
    std::vector<Chunk*> m_chunkGrid;
    
    int m_minX = 0, m_maxX = 0;
    int m_minY = 0, m_maxY = 0;
    int m_minZ = 0, m_maxZ = 0;
    int m_width = 0, m_height = 0, m_depth = 0;

    inline size_t getGridIndex(int cx, int cy, int cz) const {
        if (cx < m_minX || cx > m_maxX || cy < m_minY || cy > m_maxY || cz < m_minZ || cz > m_maxZ)
            return static_cast<size_t>(-1);
        return static_cast<size_t>((cx - m_minX) + (cy - m_minY) * m_width + (cz - m_minZ) * m_width * m_height);
    }
};

} // namespace world
