#include "ChunkStorage.hpp"
#include "ChunkRenderer.hpp"
#include <iostream>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include "../vendor/FastNoiseLite.h"

namespace world {

static void worldToChunk(int wx, int CHUNK_SZ, int& cx, int& lx) {
    cx = (wx >= 0) ? (wx / CHUNK_SZ) : ((wx - CHUNK_SZ + 1) / CHUNK_SZ);
    lx = wx - cx * CHUNK_SZ;
}

void ChunkStorage::generateWorld(int radiusX, int radiusZ, int seed) {
    clear();

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
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(3);
    noise.SetFrequency(0.03f);

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
                int h = 14 + static_cast<int>(n * 10.0f);
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

    // --- PHASE 2: Underground stubs (no fillTerrain, UNGENERATED) ---
    struct UnderTask { int cx, cy, cz; size_t idx; };
    std::vector<UnderTask> underTasks;

    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            int colIdx = (cz + radiusZ) * m_width + (cx + radiusX);
            int maxCY = columns[colIdx].maxCY;

            // Generate chunks from the bottom of the world (m_minY) up to the surface (maxCY).
            for (int cy = m_minY; cy <= maxCY; ++cy) {
                size_t idx = getGridIndex(cx, cy, cz);
                if (idx == static_cast<size_t>(-1)) continue;
                if (cy == maxCY) {
                    surfaceTasks.push_back({cx, cy, cz, idx});
                } else {
                    underTasks.push_back({cx, cy, cz, idx});
                }
            }
        }
    }

    int surfaceCount = static_cast<int>(surfaceTasks.size());
    int underCount   = static_cast<int>(underTasks.size());
    int totalCount   = surfaceCount + underCount;

    m_activeChunks.resize(totalCount);

    // --- PARALLEL ALLOCATION & GENERATION ---
    // Memory allocation (std::make_unique) for 100k+ chunks is SLOW in a single thread.
    // We must allocate ALL chunks (surface AND underground) in parallel to reach >1B voxels/sec.
    std::atomic<size_t> taskIdx{0};
    uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    
    {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t t = 0; t < numThreads; ++t) {
            threads.emplace_back([this, &surfaceTasks, &underTasks, &taskIdx, seed, surfaceCount, totalCount]() {
                while (true) {
                    size_t i = taskIdx.fetch_add(1, std::memory_order_relaxed);
                    if (i >= static_cast<size_t>(totalCount)) break;
                    
                    if (i < static_cast<size_t>(surfaceCount)) {
                        // Phase 1: Surface Chunk (allocate + fillTerrain)
                        const auto& task = surfaceTasks[i];
                        auto chunk = std::make_unique<Chunk>(task.cx, task.cy, task.cz);
                        chunk->fillTerrain(seed);
                        chunk->m_state.store(ChunkState::READY, std::memory_order_release);
                        m_chunkGrid[task.idx] = chunk.get();
                        m_activeChunks[i] = std::move(chunk);
                    } else {
                        // Phase 2: Under Chunk (allocate only, UNGENERATED)
                        size_t uIdx = i - surfaceCount;
                        const auto& task = underTasks[uIdx];
                        auto chunk = std::make_unique<Chunk>(task.cx, task.cy, task.cz);
                        // Leave as UNGENERATED
                        m_chunkGrid[task.idx] = chunk.get();
                        m_activeChunks[i] = std::move(chunk);
                    }
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

    std::cout << "[ChunkStorage] Generated " << totalCount  << " chunks ("
              << m_width << "x" << m_depth  << " grid).\n"
              << "[ChunkStorage] Surface ready: " << surfaceCount << " chunks in "
              << timeMs << " ms (" << (voxelsPerSec / 1000000.0f) << " Mvox/sec). "
              << underCount << " underground chunks deferred.\n" << std::flush;
}

void ChunkStorage::clear() {
    m_activeChunks.clear();
    m_chunkGrid.clear();
    m_width = m_height = m_depth = 0;
}

void ChunkStorage::removeChunk(int cx, int cy, int cz) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx != static_cast<size_t>(-1) && m_chunkGrid[idx]) {
        Chunk* target = m_chunkGrid[idx];
        m_chunkGrid[idx] = nullptr;
        // Remove from active chunks efficiently in O(1)
        for (size_t i = 0; i < m_activeChunks.size(); ++i) {
            if (m_activeChunks[i].get() == target) {
                std::swap(m_activeChunks[i], m_activeChunks.back());
                m_activeChunks.pop_back();
                break;
            }
        }
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

void ChunkStorage::createChunkIfMissing(int cx, int cy, int cz, int seed, ChunkRenderer& renderer, bool async) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx == static_cast<size_t>(-1)) return; // Out of bounds
    
    // Mutex was removed from ChunkStorage previously, but if needed, thread-safety for single-thread updateCamera is guaranteed.
    // ChunkManager::updateCamera is run on the main thread, so m_chunkGrid and m_activeChunks are safe.
    if (!m_chunkGrid[idx]) {
        auto chunk = std::make_unique<Chunk>(cx, cy, cz);
        
        if (!async) {
            chunk->fillTerrain(seed);
            chunk->m_state.store(ChunkState::READY, std::memory_order_release);
        } else {
            chunk->m_state.store(ChunkState::UNGENERATED, std::memory_order_release);
        }
        
        Chunk* rawPtr = chunk.get();
        m_chunkGrid[idx] = rawPtr;
        m_activeChunks.push_back(std::move(chunk));
        
        if (!async) {
            renderer.markDirty(cx, cy, cz);
        }
    } else if (!async) {
        // Якщо чанк вже існує, але він UNGENERATED, а ми просимо синхронний виклик
        Chunk* chunk = m_chunkGrid[idx];
        ChunkState expected = ChunkState::UNGENERATED;
        if (chunk->m_state.compare_exchange_strong(expected, ChunkState::READY, std::memory_order_acq_rel)) {
            chunk->fillTerrain(seed);
            renderer.markDirty(cx, cy, cz);
        }
    }
}

std::pair<int, int> ChunkStorage::getSurfaceBounds(int cx, int cz, int seed) const {
    FastNoiseLite noise;
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(3);
    noise.SetFrequency(0.03f);

    int worldBaseX = cx * CHUNK_SIZE;
    int worldBaseZ = cz * CHUNK_SIZE;

    int minHeight = 999999;
    int maxHeight = -999999;

    float nx[5] = { static_cast<float>(worldBaseX), static_cast<float>(worldBaseX + CHUNK_SIZE - 1), 
                    static_cast<float>(worldBaseX), static_cast<float>(worldBaseX + CHUNK_SIZE - 1),
                    static_cast<float>(worldBaseX + CHUNK_SIZE / 2) };
    float nz[5] = { static_cast<float>(worldBaseZ), static_cast<float>(worldBaseZ), 
                    static_cast<float>(worldBaseZ + CHUNK_SIZE - 1), static_cast<float>(worldBaseZ + CHUNK_SIZE - 1),
                    static_cast<float>(worldBaseZ + CHUNK_SIZE / 2) };
    
    for (int i = 0; i < 5; ++i) {
        float n = noise.GetNoise(nx[i], nz[i]);
        int h = 14 + static_cast<int>(n * 10.0f);
        if (h - 2 < minHeight) minHeight = h - 2;
        if (h + 2 > maxHeight) maxHeight = h + 2;
    }

    int minCY = std::floor(static_cast<float>(minHeight) / CHUNK_SIZE);
    int maxCY = std::floor(static_cast<float>(maxHeight) / CHUNK_SIZE);
    
    return {std::max(minCY, m_minY), std::min(maxCY, m_maxY)};
}

int ChunkStorage::getSurfaceMidY(int cx, int cz) const {
    auto [minCY, maxCY] = getSurfaceBounds(cx, cz, 42);
    return (minCY + maxCY) / 2;
}

} // namespace world
