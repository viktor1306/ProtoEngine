#include "ChunkStorage.hpp"
#include "ChunkRenderer.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>
#include <unordered_set>
#include "../vendor/FastNoiseLite.h"

namespace world {

static void worldToChunk(int wx, int CHUNK_SZ, int& cx, int& lx) {
    cx = (wx >= 0) ? (wx / CHUNK_SZ) : ((wx - CHUNK_SZ + 1) / CHUNK_SZ);
    lx = wx - cx * CHUNK_SZ;
}

void ChunkStorage::addActiveChunk(int cx, int cy, int cz) {
    IVec3Key key{cx, cy, cz};
    if (m_activeChunkIndices.find(key) != m_activeChunkIndices.end()) {
        return;
    }

    const size_t newIndex = m_activeChunks.size();
    m_activeChunks.push_back({cx, cy, cz});
    m_activeChunkIndices.emplace(key, newIndex);
}

void ChunkStorage::eraseActiveChunk(const IVec3Key& key) {
    auto indexIt = m_activeChunkIndices.find(key);
    if (indexIt == m_activeChunkIndices.end()) {
        return;
    }

    const size_t removeIndex = indexIt->second;
    const size_t lastIndex = m_activeChunks.size() - 1;

    if (removeIndex != lastIndex) {
        const ActiveChunk moved = m_activeChunks[lastIndex];
        m_activeChunks[removeIndex] = moved;
        m_activeChunkIndices[IVec3Key{moved.cx, moved.cy, moved.cz}] = removeIndex;
    }

    m_activeChunks.pop_back();
    m_activeChunkIndices.erase(indexIt);
}

void ChunkStorage::clear() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_activeChunks.clear();
    m_activeChunkIndices.clear();
    m_chunkRegistry.clear();
    m_chunkGrid.clear();
    m_dirtyCache.clear();
    m_boundsCache.clear(); // Invalidate lazy bounds cache on world reset
}

void ChunkStorage::generateWorld(int radiusX, int radiusZ, const TerrainConfig& config) {
    clear();

    // Capture the exact terrain config for later column-bound queries and chunk rehydration.
    m_cachedConfig = config;

    m_minX = -radiusX;
    m_maxX =  radiusX;
    // Dynamic Y bounds derived from terrain config.
    // Minimum: ocean floor is seaLevel-20; chunk 0 is always included for safety.
    m_minY = 0;
    // Maximum: tallest possible peak accounts for mountain noise boosting heights.
    // Mountain formula peak = baseHeight + amplitude * mountainStrength * 1.95
    // We take the larger of plain-amplitude vs mountain-amplitude and add 2 chunk safety margin.
    const int maxBlockHeight = config.baseHeight
        + static_cast<int>(std::max(config.amplitude,
                                    config.amplitude * config.mountainStrength * 1.95f)) + 10;
    m_maxY = (maxBlockHeight + CHUNK_SIZE - 1) / CHUNK_SIZE + 2;
    m_maxY = std::max(m_maxY, 6); // at least 6 chunks (192 blocks) tall
    m_minZ = -radiusZ;
    m_maxZ =  radiusZ;
    
    m_width = m_maxX - m_minX + 1;
    m_height = m_maxY - m_minY + 1;
    m_depth = m_maxZ - m_minZ + 1;

    size_t totalGridSize = static_cast<size_t>(m_width) * m_height * m_depth;
    m_chunkGrid.assign(totalGridSize, nullptr);

    auto t0 = std::chrono::high_resolution_clock::now();
    
    // Estimate occupied Y extents per (cx,cz) column before allocating chunks.
    struct ColumnData { int minCY, maxCY; };
    std::vector<ColumnData> columns(m_width * m_depth);

    FastNoiseLite noise;
    noise.SetSeed(config.seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(config.octaves);
    noise.SetFrequency(config.frequency);

    // Simplified radial island mask (no warp noise here; this is a conservative bounds pass).
    const float rBlks    = (config.islandMode && config.worldRadiusBlks > 0)
                           ? static_cast<float>(config.worldRadiusBlks) : 1e9f;
    const float falloff  = config.islandFalloff;
    const float oceanFlH = static_cast<float>(config.seaLevel - 20);
    auto islandMaskSimple = [&](float wx, float wz) -> float {
        if (!config.islandMode) return 1.0f;
        float dx = wx / rBlks, dz = wz / rBlks;
        float dist = std::sqrt(dx*dx + dz*dz);
        float t = (dist - falloff) / (1.1f - falloff);
        t = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    };

    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            const int worldBaseX = cx * CHUNK_SIZE;
            const int worldBaseZ = cz * CHUNK_SIZE;
            int minH = 999999, maxH = -999999;

            float sampleX[5] = {
                (float)worldBaseX,               (float)(worldBaseX + CHUNK_SIZE - 1),
                (float)worldBaseX,               (float)(worldBaseX + CHUNK_SIZE - 1),
                (float)(worldBaseX + CHUNK_SIZE / 2)
            };
            float sampleZ[5] = {
                (float)worldBaseZ,               (float)worldBaseZ,
                (float)(worldBaseZ + CHUNK_SIZE - 1), (float)(worldBaseZ + CHUNK_SIZE - 1),
                (float)(worldBaseZ + CHUNK_SIZE / 2)
            };

            for (int i = 0; i < 5; ++i) {
                // Apply worldScale: divide coords so GetNoise sees effective lower frequency
                float n     = noise.GetNoise(sampleX[i] / config.worldScale,
                                             sampleZ[i] / config.worldScale);
                float landH = static_cast<float>(config.baseHeight) + n * config.amplitude;
                float mask  = islandMaskSimple(sampleX[i], sampleZ[i]);
                float h     = oceanFlH + (landH - oceanFlH) * mask;

                // Mountain noise (erosion) can push peaks significantly higher:
                // peak ≈ h + amplitude * mountainStrength * 0.95  (conservative estimate)
                float hMountMax = h + config.amplitude * config.mountainStrength * 0.95f;

                // Include the water surface in the occupied bounds so sea-level slices exist.
                float lo = std::min(h, (float)config.seaLevel) - 2.0f;
                float hi = std::max(hMountMax, (float)config.seaLevel) + 4.0f;
                if ((int)lo < minH) minH = (int)lo;
                if ((int)hi > maxH) maxH = (int)hi;
            }

            int colIdx = (cz + radiusZ) * m_width + (cx + radiusX);
            columns[colIdx] = {
                std::max(static_cast<int>(std::floor(static_cast<float>(minH) / CHUNK_SIZE)), m_minY),
                std::min(static_cast<int>(std::floor(static_cast<float>(maxH) / CHUNK_SIZE)), m_maxY)
            };
        }
    }

    // Allocate every occupied Y-slice in each column up front.
    // The current world model is full-column preallocation within conservative bounds,
    // not surface-only sparse generation.
    struct ChunkTask { int cx, cy, cz; size_t idx; };
    std::vector<ChunkTask> allTasks;
    allTasks.reserve(static_cast<size_t>(m_width) * m_depth * 4); // ~4 Y slices on avg

    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            int colIdx = (cz + radiusZ) * m_width + (cx + radiusX);
            int minCY  = columns[colIdx].minCY;
            int maxCY  = columns[colIdx].maxCY;

            // Allocate every occupied slice so streaming later only rehydrates evicted chunks.
            for (int cy = minCY; cy <= maxCY; ++cy) {
                size_t idx = getGridIndex(cx, cy, cz);
                if (idx != static_cast<size_t>(-1))
                    allTasks.push_back({cx, cy, cz, idx});
            }
        }
    }

    struct GeneratedChunkRecord {
        int cx, cy, cz;
        size_t idx;
        std::unique_ptr<Chunk> chunk;
    };

    int totalCount = static_cast<int>(allTasks.size());
    std::vector<GeneratedChunkRecord> generated(static_cast<size_t>(totalCount));

    // Parallel allocation + fillTerrain across the full occupied chunk set.
    std::atomic<size_t> taskIdx{0};
    uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency());

    {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t t = 0; t < numThreads; ++t) {
            threads.emplace_back([this, &allTasks, &generated, &taskIdx, config, totalCount]() {
                while (true) {
                    size_t i = taskIdx.fetch_add(1, std::memory_order_relaxed);
                    if (i >= static_cast<size_t>(totalCount)) break;

                    const auto& task = allTasks[i];
                    auto chunk = std::make_unique<Chunk>(task.cx, task.cy, task.cz);
                    chunk->fillTerrain(config, nullptr);
                    chunk->m_state.store(ChunkState::READY, std::memory_order_release);
                    generated[i] = {task.cx, task.cy, task.cz, task.idx, std::move(chunk)};
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    m_activeChunks.clear();
    m_activeChunks.reserve(static_cast<size_t>(totalCount));
    m_activeChunkIndices.clear();
    m_activeChunkIndices.reserve(static_cast<size_t>(totalCount));
    m_chunkRegistry.clear();
    m_chunkRegistry.reserve(static_cast<size_t>(totalCount));

    for (auto& record : generated) {
        IVec3Key key{record.cx, record.cy, record.cz};
        Chunk* rawPtr = record.chunk.get();
        m_chunkGrid[record.idx] = rawPtr;
        addActiveChunk(record.cx, record.cy, record.cz);
        m_chunkRegistry.emplace(key, std::move(record.chunk));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float timeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    uint64_t voxelsPerChunk = static_cast<uint64_t>(CHUNK_SIZE) * CHUNK_SIZE * CHUNK_SIZE;
    uint64_t totalVoxels    = static_cast<uint64_t>(totalCount) * voxelsPerChunk;
    float timeSec = timeMs / 1000.0f;
    float voxelsPerSec = (timeSec > 0.0f) ? (totalVoxels / timeSec) : 0.0f;

    std::cout << "[ChunkStorage] Generated " << totalCount << " chunks ("
              << m_width << "x" << m_depth << " columns, up to "
              << (m_maxY - m_minY + 1) << " Y-slices each) in "
              << timeMs << " ms (" << (voxelsPerSec / 1000000.0f) << " Mvox/sec).\n" << std::flush;
}

void ChunkStorage::removeChunk(int cx, int cy, int cz) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx != static_cast<size_t>(-1) && m_chunkGrid[idx]) {
        m_chunkGrid[idx] = nullptr;
        const IVec3Key key{cx, cy, cz};
        m_chunkRegistry.erase(key);
        eraseActiveChunk(key);
    }
}

void ChunkStorage::removeChunks(const std::vector<IVec3Key>& keys) {
    if (keys.empty()) return;

    for (const auto& key : keys) {
        size_t idx = getGridIndex(key.x, key.y, key.z);
        if (idx != static_cast<size_t>(-1)) {
            m_chunkGrid[idx] = nullptr;
        }
    }

    for (const auto& key : keys) {
        auto registryIt = m_chunkRegistry.find(key);
        if (registryIt == m_chunkRegistry.end()) {
            eraseActiveChunk(key);
            continue;
        }

        Chunk* chunk = registryIt->second.get();
        if (chunk && chunk->m_isModified.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_dirtyCache[key] = std::move(registryIt->second);
            m_chunkRegistry.erase(registryIt);
        } else {
            m_chunkRegistry.erase(registryIt);
        }

        eraseActiveChunk(key);
    }
}

VoxelData ChunkStorage::getVoxel(int wx, int wy, int wz) const {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    const Chunk* chunk = getChunk(cx, cy, cz);
    if (!chunk) return VOXEL_AIR;
    return chunk->getVoxel(lx, ly, lz);
}

void ChunkStorage::setVoxel(int wx, int wy, int wz, VoxelData v) {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    Chunk* chunk = getChunk(cx, cy, cz);
    if (!chunk) return;
    
    chunk->setVoxel(lx, ly, lz, v);
}

const Chunk* ChunkStorage::getChunk(int cx, int cy, int cz) const {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx == static_cast<size_t>(-1)) return nullptr;
    return m_chunkGrid[idx];
}

Chunk* ChunkStorage::getChunk(int cx, int cy, int cz) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx == static_cast<size_t>(-1)) return nullptr;
    return m_chunkGrid[idx];
}

void ChunkStorage::createChunkIfMissing(int cx, int cy, int cz, const TerrainConfig& config, ChunkRenderer& renderer, bool /*async*/) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx == static_cast<size_t>(-1)) return; // Out of bounds

    if (!m_chunkGrid[idx]) {
        IVec3Key k{cx, cy, cz};
        std::unique_ptr<Chunk> cachedChunk;
        
        // Modified chunks survive Tier-4 eviction here and must be restored verbatim.
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            auto it = m_dirtyCache.find(k);
            if (it != m_dirtyCache.end()) {
                cachedChunk = std::move(it->second);
                m_dirtyCache.erase(it);
            }
        }

        if (cachedChunk) {
            Chunk* rawPtr = cachedChunk.get();
            m_chunkGrid[idx] = rawPtr;
            addActiveChunk(cx, cy, cz);
            m_chunkRegistry[IVec3Key{cx, cy, cz}] = std::move(cachedChunk);
            
            // Restored chunks already contain voxel payload; do not regenerate and overwrite player edits.
            // ChunkManager will assign render state on the next updateCamera() pass.
            rawPtr->markDirty();
            return;
        }

        auto chunk = std::make_unique<Chunk>(cx, cy, cz);
        chunk->m_state.store(ChunkState::UNGENERATED, std::memory_order_release);

        Chunk* rawPtr = chunk.get();
        m_chunkGrid[idx] = rawPtr;
        addActiveChunk(cx, cy, cz);
        m_chunkRegistry[IVec3Key{cx, cy, cz}] = std::move(chunk);

        // Re-created chunks re-enter as placeholders first and are then generated asynchronously.
        ChunkState expected = ChunkState::UNGENERATED;
        if (rawPtr->m_state.compare_exchange_strong(expected, ChunkState::GENERATING,
                                                    std::memory_order_acq_rel)) {
            renderer.submitGenerateTaskLow(rawPtr, config);
        }
    } else {
        // Chunk exists in storage but may still be a placeholder after stream re-entry.
        // Try to claim generation if no worker has started it yet.
        Chunk* chunkPtr = m_chunkGrid[idx];
        ChunkState expected = ChunkState::UNGENERATED;
        if (chunkPtr->m_state.compare_exchange_strong(expected, ChunkState::GENERATING,
                                                      std::memory_order_acq_rel)) {
            renderer.submitGenerateTaskLow(chunkPtr, config);
        }
    }
}


std::pair<int, int> ChunkStorage::getSurfaceBounds(int cx, int cz) const {
    // Fast path: O(1) cache hit — bounds are immutable for a fixed TerrainConfig.
    const uint64_t cacheKey = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                             |  static_cast<uint64_t>(static_cast<uint32_t>(cz));
    {
        auto it = m_boundsCache.find(cacheKey);
        if (it != m_boundsCache.end()) return it->second;
    }

    // Slow path: recompute the occupied Y-range for this column exactly once per world instance.
    // thread_local: re-init when seed OR worldScale changes.
    static thread_local FastNoiseLite tl_noise;
    static thread_local int           tl_seed  = std::numeric_limits<int>::min();
    static thread_local float         tl_scale = -1.0f;

    if (tl_seed != m_cachedConfig.seed || tl_scale != m_cachedConfig.worldScale) {
        tl_noise.SetSeed(m_cachedConfig.seed);
        tl_noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        tl_noise.SetFractalType(FastNoiseLite::FractalType_FBm);
        tl_noise.SetFractalOctaves(m_cachedConfig.octaves);
        tl_noise.SetFrequency(m_cachedConfig.frequency);
        tl_seed  = m_cachedConfig.seed;
        tl_scale = m_cachedConfig.worldScale;
    }

    const float rBlks   = (m_cachedConfig.islandMode && m_cachedConfig.worldRadiusBlks > 0)
                          ? static_cast<float>(m_cachedConfig.worldRadiusBlks) : 1e9f;
    const float falloff = m_cachedConfig.islandFalloff;
    const float oceanFl = static_cast<float>(m_cachedConfig.seaLevel - 20);
    const float wScale  = (m_cachedConfig.worldScale > 0.0f) ? m_cachedConfig.worldScale : 1.0f;

    auto islandMaskSimple = [&](float wx, float wz) -> float {
        if (!m_cachedConfig.islandMode) return 1.0f;
        float dx = wx / rBlks, dz = wz / rBlks;
        float dist = std::sqrt(dx*dx + dz*dz);
        float t = (dist - falloff) / (1.1f - falloff);
        t = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    };

    const int worldBaseX = cx * CHUNK_SIZE;
    const int worldBaseZ = cz * CHUNK_SIZE;
    int minH = 999999, maxH = -999999;

    std::array<float, 5> sampleX = { (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                                     (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                                     (float)(worldBaseX + CHUNK_SIZE / 2) };
    std::array<float, 5> sampleZ = { (float)worldBaseZ, (float)worldBaseZ,
                                     (float)(worldBaseZ + CHUNK_SIZE - 1), (float)(worldBaseZ + CHUNK_SIZE - 1),
                                     (float)(worldBaseZ + CHUNK_SIZE / 2) };

    for (int i = 0; i < 5; ++i) {
        float n     = tl_noise.GetNoise(sampleX[i] / wScale, sampleZ[i] / wScale);
        float landH = static_cast<float>(m_cachedConfig.baseHeight) + n * m_cachedConfig.amplitude;
        float mask  = islandMaskSimple(sampleX[i], sampleZ[i]);
        float h     = oceanFl + (landH - oceanFl) * mask;

        // Conservative mountain peak estimate
        float hMountMax = h + m_cachedConfig.amplitude * m_cachedConfig.mountainStrength * 0.95f;

        float lo = std::min(h, (float)m_cachedConfig.seaLevel) - 2.0f;
        float hi = std::max(hMountMax, (float)m_cachedConfig.seaLevel) + 4.0f;
        if ((int)lo < minH) minH = (int)lo;
        if ((int)hi > maxH) maxH = (int)hi;
    }

    int minCY = static_cast<int>(std::floor(static_cast<float>(minH) / CHUNK_SIZE));
    int maxCY = static_cast<int>(std::floor(static_cast<float>(maxH) / CHUNK_SIZE));

    auto result = std::make_pair(std::max(minCY, m_minY), std::min(maxCY, m_maxY));
    m_boundsCache[cacheKey] = result;
    return result;
}

int ChunkStorage::getSurfaceMidY(int cx, int cz) const {
    auto [minCY, maxCY] = getSurfaceBounds(cx, cz);
    return (minCY + maxCY) / 2;
}

} // namespace world
