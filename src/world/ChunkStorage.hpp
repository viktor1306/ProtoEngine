#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <mutex>
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
    void generateWorld(int radiusX, int radiusZ, const TerrainConfig& config = {});
    void clear();
    void removeChunk(int cx, int cy, int cz);
    void removeChunks(const std::vector<IVec3Key>& keys);
    
    void createChunkIfMissing(int cx, int cy, int cz, const TerrainConfig& config, ChunkRenderer& renderer, bool async = false);

    VoxelData getVoxel(int wx, int wy, int wz) const;
    void      setVoxel(int wx, int wy, int wz, VoxelData v);

    const Chunk* getChunk(int cx, int cy, int cz) const;
    Chunk*       getChunk(int cx, int cy, int cz);

    // Використовують кешований noise (ініціалізований у generateWorld)
    std::pair<int, int> getSurfaceBounds(int cx, int cz) const;
    int                 getSurfaceMidY  (int cx, int cz) const;

    // Доступ до кешованого конфігу (потрібен ChunkManager для передачі в createChunkIfMissing)
    const TerrainConfig& getCachedConfig() const { return m_cachedConfig; }

    struct ActiveChunk {
        int cx, cy, cz;
        std::unique_ptr<Chunk> chunk;
    };

    const std::vector<ActiveChunk>& getChunks() const { return m_activeChunks; }
    std::vector<ActiveChunk>&       getChunks()       { return m_activeChunks; }

    // World grid bounds (in chunk coords)
    int getMinX() const { return m_minX; }
    int getMaxX() const { return m_maxX; }
    int getMinY() const { return m_minY; }
    int getMaxY() const { return m_maxY; }
    int getMinZ() const { return m_minZ; }
    int getMaxZ() const { return m_maxZ; }

private:
    std::vector<ActiveChunk> m_activeChunks;
    std::vector<Chunk*> m_chunkGrid;
    
    // --- RAM Cache for modified chunks ---
    std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash> m_dirtyCache;
    std::mutex m_cacheMutex;
    
    int m_minX = 0, m_maxX = 0;
    int m_minY = 0, m_maxY = 0;
    int m_minZ = 0, m_maxZ = 0;
    int m_width = 0, m_height = 0, m_depth = 0;

    // Cached config — set once in generateWorld, used by getSurfaceBounds.
    TerrainConfig m_cachedConfig;

    // Lazy surface-bounds cache: each column (cx,cz) computed at most once per session.
    // Terrain is deterministic, so bounds never change after generateWorld.
    // Key = (uint32(cx) << 32) | uint32(cz).
    mutable std::unordered_map<uint64_t, std::pair<int, int>> m_boundsCache;

    inline size_t getGridIndex(int cx, int cy, int cz) const {
        if (cx < m_minX || cx > m_maxX || cy < m_minY || cy > m_maxY || cz < m_minZ || cz > m_maxZ)
            return static_cast<size_t>(-1);
        return static_cast<size_t>((cx - m_minX) + (cy - m_minY) * m_width + (cz - m_minZ) * m_width * m_height);
    }
};

}
