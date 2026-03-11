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

    // Legacy name: returns the occupied Y-range for a chunk column, not only the visible surface slice.
    // Uses cached terrain config captured by generateWorld().
    std::pair<int, int> getSurfaceBounds(int cx, int cz) const;
    int                 getSurfaceMidY  (int cx, int cz) const;

    // Access to the terrain config captured by the last generateWorld() call.
    const TerrainConfig& getCachedConfig() const { return m_cachedConfig; }

    struct ActiveChunk {
        int cx, cy, cz;
    };

    const std::vector<ActiveChunk>& getChunks() const { return m_activeChunks; }
    std::vector<ActiveChunk>&       getChunks()       { return m_activeChunks; }
    size_t getDirtyCacheCount() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        return m_dirtyCache.size();
    }

    // World grid bounds (in chunk coords)
    int getMinX() const { return m_minX; }
    int getMaxX() const { return m_maxX; }
    int getMinY() const { return m_minY; }
    int getMaxY() const { return m_maxY; }
    int getMinZ() const { return m_minZ; }
    int getMaxZ() const { return m_maxZ; }

private:
    using ChunkRegistry = std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash>;

    void addActiveChunk(int cx, int cy, int cz);
    void eraseActiveChunk(const IVec3Key& key);

    // Lightweight iteration list for streaming / LOD passes.
    std::vector<ActiveChunk> m_activeChunks;
    // Coordinate -> index mapping for O(1) swap-pop removal from m_activeChunks.
    std::unordered_map<IVec3Key, size_t, IVec3Hash> m_activeChunkIndices;
    // Authoritative ownership of active chunk objects.
    ChunkRegistry m_chunkRegistry;
    std::vector<Chunk*> m_chunkGrid;
    
    // Tier-4 eviction cache: modified chunks are parked here so stream-out does not lose player edits.
    ChunkRegistry m_dirtyCache;
    mutable std::mutex m_cacheMutex;
    
    int m_minX = 0, m_maxX = 0;
    int m_minY = 0, m_maxY = 0;
    int m_minZ = 0, m_maxZ = 0;
    int m_width = 0, m_height = 0, m_depth = 0;

    // Cached config from the current world instance. Used by lazy column-bound queries and rehydration.
    TerrainConfig m_cachedConfig;

    // Lazy column-bounds cache: each (cx,cz) column is computed at most once per world instance.
    // Terrain is deterministic for a fixed TerrainConfig, so these bounds stay immutable until generateWorld().
    // Key = (uint32(cx) << 32) | uint32(cz).
    mutable std::unordered_map<uint64_t, std::pair<int, int>> m_boundsCache;

    inline size_t getGridIndex(int cx, int cy, int cz) const {
        if (cx < m_minX || cx > m_maxX || cy < m_minY || cy > m_maxY || cz < m_minZ || cz > m_maxZ)
            return static_cast<size_t>(-1);
        return static_cast<size_t>((cx - m_minX) + (cy - m_minY) * m_width + (cz - m_minZ) * m_width * m_height);
    }
};

}
