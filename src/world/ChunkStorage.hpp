#pragma once
#include <unordered_map>
#include <memory>
#include "world/Chunk.hpp"

namespace world {

// Use the same hashing as before
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

class ChunkStorage {
public:
    void generateWorld(int radiusX, int radiusZ, int seed = 42);
    void clear();

    VoxelData getVoxel(int wx, int wy, int wz) const;
    void      setVoxel(int wx, int wy, int wz, VoxelData v);

    const Chunk* getChunk(int cx, int cy, int cz) const;
    Chunk*       getChunk(int cx, int cy, int cz);

    const std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash>& getChunks() const { return m_chunks; }
    std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash>&       getChunks()       { return m_chunks; }

    int getWorldBiasX() const { return m_worldBiasX; }
    int getWorldBiasY() const { return m_worldBiasY; }
    int getWorldBiasZ() const { return m_worldBiasZ; }

private:
    std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash> m_chunks;
    int m_worldBiasX = 0;
    int m_worldBiasY = 0;
    int m_worldBiasZ = 0;
};

} // namespace world
