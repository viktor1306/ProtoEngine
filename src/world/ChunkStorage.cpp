#include "ChunkStorage.hpp"
#include "ChunkRenderer.hpp"
#include <iostream>
#include <chrono>
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

void ChunkStorage::clear() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_activeChunks.clear();
    m_chunkGrid.clear();
    m_dirtyCache.clear();
    m_boundsCache.clear(); // Invalidate lazy bounds cache on world reset
}

void ChunkStorage::generateWorld(int radiusX, int radiusZ, const TerrainConfig& config) {
    clear();

    // Cache config — used by getSurfaceBounds for lazy noise computation.
    m_cachedConfig = config;

    m_minX = -radiusX;
    m_maxX = radiusX;
    m_minY = -8;
    m_maxY = 8;
    m_minZ = -radiusZ;
    m_maxZ = radiusZ;
    
    m_width = m_maxX - m_minX + 1;
    m_height = m_maxY - m_minY + 1;
    m_depth = m_maxZ - m_minZ + 1;

    size_t totalGridSize = static_cast<size_t>(m_width) * m_height * m_depth;
    m_chunkGrid.assign(totalGridSize, nullptr);

    auto t0 = std::chrono::high_resolution_clock::now();
    
    // Collect surface columns first (cheap noise estimate per column)
    struct ColumnData { int minCY, maxCY; };
    std::vector<ColumnData> columns(m_width * m_depth);

    FastNoiseLite noise;
    noise.SetSeed(config.seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(config.octaves);
    noise.SetFrequency(config.frequency);

    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            int worldBaseX = cx * CHUNK_SIZE;
            int worldBaseZ = cz * CHUNK_SIZE;
            int minH = 999999, maxH = -999999;
            float nx[5] = { (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                            (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                            (float)(worldBaseX + CHUNK_SIZE / 2) };
            float nz[5] = { (float)worldBaseZ, (float)worldBaseZ,
                            (float)(worldBaseZ + CHUNK_SIZE - 1), (float)(worldBaseZ + CHUNK_SIZE - 1),
                            (float)(worldBaseZ + CHUNK_SIZE / 2) };
            for (int i = 0; i < 5; ++i) {
                float n = noise.GetNoise(nx[i], nz[i]);
                int h = config.baseHeight + static_cast<int>(n * config.amplitude);
                if (h - 2 < minH) minH = h - 2;
                if (h + 2 > maxH) maxH = h + 2;
            }
            int colIdx = (cz + radiusZ) * m_width + (cx + radiusX);
            columns[colIdx] = {
                std::max((int)std::floor((float)minH / CHUNK_SIZE), m_minY),
                std::min((int)std::floor((float)maxH / CHUNK_SIZE), m_maxY)
            };
        }
    }

    // --- PHASE 1: Surface chunks only (cy == maxCY per column) ---
    // These get fillTerrain immediately — fast, ~1/8 of all chunks.
    struct SurfaceTask { int cx, cy, cz; size_t idx; };
    std::vector<SurfaceTask> surfaceTasks;
    surfaceTasks.reserve(m_width * m_depth);

    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            int colIdx = (cz + radiusZ) * m_width + (cx + radiusX);
            int maxCY = columns[colIdx].maxCY;

            // Sparse Allocation: Only allocate the surface chunk!
            // Underground chunks remain nullptr until ChunkManager specifically requests them asynchronously.
            size_t idx = getGridIndex(cx, maxCY, cz);
            if (idx != static_cast<size_t>(-1)) {
                surfaceTasks.push_back({cx, maxCY, cz, idx});
            }
        }
    }

    int surfaceCount = static_cast<int>(surfaceTasks.size());
    m_activeChunks.resize(surfaceCount);

    // --- PARALLEL ALLOCATION & GENERATION ---
    std::atomic<size_t> taskIdx{0};
    uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    
    {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t t = 0; t < numThreads; ++t) {
            threads.emplace_back([this, &surfaceTasks, &taskIdx, config, surfaceCount]() {
                FastNoiseLite noise;
                noise.SetSeed(config.seed);
                noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
                noise.SetFractalType(FastNoiseLite::FractalType_FBm);
                noise.SetFractalOctaves(config.octaves);
                noise.SetFrequency(config.frequency);
                
                while (true) {
                    size_t i = taskIdx.fetch_add(1, std::memory_order_relaxed);
                    if (i >= static_cast<size_t>(surfaceCount)) break;
                    
                    const auto& task = surfaceTasks[i];
                    auto chunk = std::make_unique<Chunk>(task.cx, task.cy, task.cz);
                    chunk->fillTerrain(config, &noise);
                    chunk->m_state.store(ChunkState::READY, std::memory_order_release);
                    m_chunkGrid[task.idx] = chunk.get();
                    m_activeChunks[i] = std::move(chunk);
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float timeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    uint64_t voxelsPerChunk = static_cast<uint64_t>(CHUNK_SIZE) * CHUNK_SIZE * CHUNK_SIZE;
    uint64_t surfaceVoxels  = static_cast<uint64_t>(surfaceCount) * voxelsPerChunk;
    float timeSec = timeMs / 1000.0f;
    float voxelsPerSec = (timeSec > 0.0f) ? (surfaceVoxels / timeSec) : 0.0f;

    std::cout << "[ChunkStorage] Generated " << surfaceCount  << " surface chunks ("
              << m_width << "x" << m_depth  << " grid) in "
              << timeMs << " ms (" << (voxelsPerSec / 1000000.0f) << " Mvox/sec). "
              << "Underground chunks use Sparse Storage (deferred).\n" << std::flush;
}

void ChunkStorage::removeChunk(int cx, int cy, int cz) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx != static_cast<size_t>(-1) && m_chunkGrid[idx]) {
        m_chunkGrid[idx] = nullptr;
        // Remove from active chunks efficiently
        auto it = std::remove_if(m_activeChunks.begin(), m_activeChunks.end(),
        [cx, cy, cz](const std::unique_ptr<Chunk>& c) {
            return c && c->getCX() == cx && c->getCY() == cy && c->getCZ() == cz;
        });
    m_activeChunks.erase(it, m_activeChunks.end());
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

    std::unordered_set<IVec3Key, IVec3Hash> keysToRemove(keys.begin(), keys.end());

    auto it = std::remove_if(m_activeChunks.begin(), m_activeChunks.end(),
        [this, &keysToRemove](std::unique_ptr<Chunk>& c) {
            if (!c) return true;
            IVec3Key k{c->getCX(), c->getCY(), c->getCZ()};
            if (keysToRemove.find(k) != keysToRemove.end()) {
                // RAM Cache Intercept: Preserve modified chunks
                if (c->m_isModified.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> lock(m_cacheMutex);
                    m_dirtyCache[k] = std::move(c);
                }
                return true;
            }
            return false;
        });

    m_activeChunks.erase(it, m_activeChunks.end());
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
        
        // --- RAM Cache Intercept: Load modified chunks if they exist ---
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
            m_activeChunks.push_back(std::move(cachedChunk));
            
            // The chunk is already populated from the cache.
            // Do NOT call submitGenerateTaskHigh, because that forces a GENERATE task 
            // which runs fillTerrain and overwrites our restored modifications!
            // We just let ChunkManager naturally assign an LOD and mark it dirty next frame.
            rawPtr->markDirty();
            return;
        }

        auto chunk = std::make_unique<Chunk>(cx, cy, cz);
        chunk->m_state.store(ChunkState::UNGENERATED, std::memory_order_release);

        Chunk* rawPtr = chunk.get();
        m_chunkGrid[idx] = rawPtr;
        m_activeChunks.push_back(std::move(chunk));

        // Immediately submit a low-priority background generation task.
        ChunkState expected = ChunkState::UNGENERATED;
        if (rawPtr->m_state.compare_exchange_strong(expected, ChunkState::GENERATING,
                                                    std::memory_order_acq_rel)) {
            renderer.submitGenerateTaskLow(rawPtr, config);
        }
    } else {
        // Chunk exists in storage but may still be UNGENERATED (e.g. Tier-4 eviction
        // re-entered the sphere before the previous generate task finished).
        // Try to claim it: if still UNGENERATED, submit a new generate task.
        Chunk* chunkPtr = m_chunkGrid[idx];
        ChunkState expected = ChunkState::UNGENERATED;
        if (chunkPtr->m_state.compare_exchange_strong(expected, ChunkState::GENERATING,
                                                      std::memory_order_acq_rel)) {
            renderer.submitGenerateTaskLow(chunkPtr, config);
        }
    }
}


std::pair<int, int> ChunkStorage::getSurfaceBounds(int cx, int cz) const {
    // Fast path: O(1) cache hit — terrain is deterministic, bounds are immutable.
    const uint64_t cacheKey = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                             |  static_cast<uint64_t>(static_cast<uint32_t>(cz));
    {
        auto it = m_boundsCache.find(cacheKey);
        if (it != m_boundsCache.end()) return it->second;
    }

    // Slow path: compute from noise (exactly once per (cx,cz) per session).
    // thread_local: re-init only when seed changes (e.g. after Rebuild World).
    static thread_local FastNoiseLite tl_noise;
    static thread_local int           tl_seed = std::numeric_limits<int>::min();
    if (tl_seed != m_cachedConfig.seed) {
        tl_noise.SetSeed(m_cachedConfig.seed);
        tl_noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        tl_noise.SetFractalType(FastNoiseLite::FractalType_FBm);
        tl_noise.SetFractalOctaves(m_cachedConfig.octaves);
        tl_noise.SetFrequency(m_cachedConfig.frequency);
        tl_seed = m_cachedConfig.seed;
    }

    int worldBaseX = cx * CHUNK_SIZE;
    int worldBaseZ = cz * CHUNK_SIZE;
    int minH = 999999, maxH = -999999;

    std::array<float, 5> nx = { (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                                (float)worldBaseX, (float)(worldBaseX + CHUNK_SIZE - 1),
                                (float)(worldBaseX + CHUNK_SIZE / 2) };
    std::array<float, 5> nz = { (float)worldBaseZ, (float)worldBaseZ,
                                (float)(worldBaseZ + CHUNK_SIZE - 1), (float)(worldBaseZ + CHUNK_SIZE - 1),
                                (float)(worldBaseZ + CHUNK_SIZE / 2) };

    for (int i = 0; i < 5; ++i) {
        float n = tl_noise.GetNoise(nx[i], nz[i]);
        int h = m_cachedConfig.baseHeight + static_cast<int>(n * m_cachedConfig.amplitude);
        if (h - 2 < minH) minH = h - 2;
        if (h + 2 > maxH) maxH = h + 2;
    }

    int minCY = std::floor(static_cast<float>(minH) / CHUNK_SIZE);
    int maxCY = std::floor(static_cast<float>(maxH) / CHUNK_SIZE);

    auto result = std::make_pair(std::max(minCY, m_minY), std::min(maxCY, m_maxY));
    m_boundsCache[cacheKey] = result;
    return result;
}

int ChunkStorage::getSurfaceMidY(int cx, int cz) const {
    auto [minCY, maxCY] = getSurfaceBounds(cx, cz);
    return (minCY + maxCY) / 2;
}

} // namespace world
