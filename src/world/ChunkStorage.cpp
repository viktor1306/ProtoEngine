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
    m_activeChunks.reserve(totalGridSize);

    auto t0 = std::chrono::high_resolution_clock::now();
    
    // 1. Збираємо координати для генерації (швидко, без алокацій пам'яті чанків)
    struct TaskData { int cx, cy, cz; size_t idx; };
    std::vector<TaskData> tasks;
    tasks.reserve(m_width * m_depth * 2);

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

            int minHeight = 999999;
            int maxHeight = -999999;

            // Оптимізація: генеруємо шум лише для кутів чанка + центру (швидка апроксимація)
            float nx[5] = { static_cast<float>(worldBaseX), static_cast<float>(worldBaseX + CHUNK_SIZE - 1), 
                            static_cast<float>(worldBaseX), static_cast<float>(worldBaseX + CHUNK_SIZE - 1),
                            static_cast<float>(worldBaseX + CHUNK_SIZE / 2) };
            float nz[5] = { static_cast<float>(worldBaseZ), static_cast<float>(worldBaseZ), 
                            static_cast<float>(worldBaseZ + CHUNK_SIZE - 1), static_cast<float>(worldBaseZ + CHUNK_SIZE - 1),
                            static_cast<float>(worldBaseZ + CHUNK_SIZE / 2) };
            
            for (int i = 0; i < 5; ++i) {
                float n = noise.GetNoise(nx[i], nz[i]);
                int h = 14 + static_cast<int>(n * 10.0f);
                // Додаємо запас +-2 блоки, бо між кутами можуть бути піки
                if (h - 2 < minHeight) minHeight = h - 2;
                if (h + 2 > maxHeight) maxHeight = h + 2;
            }

            int minCY = std::floor(static_cast<float>(minHeight) / CHUNK_SIZE);
            int maxCY = std::floor(static_cast<float>(maxHeight) / CHUNK_SIZE);

            for (int cy = m_minY; cy <= m_maxY; ++cy) {
                // Додаємо чанк, якщо він хоча б частково перетинає діапазон рельєфу (включаючи bedrock до minCY)
                // Або якщо він нижче поверхні, він буде згенерований пізніше. Зараз нас цікавлять ТІЛЬКИ 
                // чанки від minCY до maxCY включно, щоб ми бачили поверхню.
                if (cy >= minCY && cy <= maxCY) {
                    size_t idx = getGridIndex(cx, cy, cz);
                    if (idx != static_cast<size_t>(-1)) {
                        tasks.push_back({cx, cy, cz, idx});
                    }
                }
            }
        }
    }

    int count = static_cast<int>(tasks.size());
    m_activeChunks.resize(count);

    // 2. Паралельна генерація ТА ВИДІЛЕННЯ ПАМ'ЯТІ
    std::atomic<size_t> currentTask{0};
    uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (uint32_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &tasks, &currentTask, seed, count]() {
            while (true) {
                size_t taskIdx = currentTask.fetch_add(1, std::memory_order_relaxed);
                if (taskIdx >= static_cast<size_t>(count)) break;
                
                const auto& task = tasks[taskIdx];
                
                // Виділення пам'яті тепер теж паралельне! ОС ефективніше утилізує ресурси.
                auto chunk = std::make_unique<Chunk>(task.cx, task.cy, task.cz);
                chunk->fillTerrain(seed);
                chunk->m_state.store(ChunkState::READY, std::memory_order_release);
                
                m_chunkGrid[task.idx] = chunk.get();
                m_activeChunks[taskIdx] = std::move(chunk);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float timeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    
    // Чанк має розмір 32x32x32 (CHUNK_SIZE^3)
    uint64_t voxelsPerChunk = static_cast<uint64_t>(CHUNK_SIZE) * CHUNK_SIZE * CHUNK_SIZE;
    uint64_t totalVoxels = static_cast<uint64_t>(count) * voxelsPerChunk;
    float timeSec = timeMs / 1000.0f;
    float voxelsPerSec = (timeSec > 0.0f) ? (totalVoxels / timeSec) : 0.0f;

    std::cout << "[ChunkStorage] Generated " << count << " chunks (" 
              << m_width << "x" << m_depth << " grid).\n"
              << "[ChunkStorage] Space Volume: " << totalVoxels << " voxels (" << CHUNK_SIZE << "^3 per chunk) in " 
              << timeMs << " ms (" << (voxelsPerSec / 1000000.0f) << " Million voxels/sec).\n" 
              << std::flush;
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
        // Remove from active chunks
        for (auto it = m_activeChunks.begin(); it != m_activeChunks.end(); ++it) {
            if (it->get() == target) {
                m_activeChunks.erase(it);
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

void ChunkStorage::createChunkIfMissing(int cx, int cy, int cz, int seed, ChunkRenderer& renderer) {
    size_t idx = getGridIndex(cx, cy, cz);
    if (idx == static_cast<size_t>(-1)) return; // Out of bounds
    
    if (!m_chunkGrid[idx]) {
        // Create new empty chunk (UNGENERATED state by default)
        auto chunk = std::make_unique<Chunk>(cx, cy, cz);
        
        ChunkState expected = ChunkState::UNGENERATED;
        if (chunk->m_state.compare_exchange_strong(expected, ChunkState::GENERATING, std::memory_order_acq_rel)) {
            m_chunkGrid[idx] = chunk.get();
            renderer.submitGenerateTaskHigh(chunk.get(), seed);
            
            // Note: In a fully concurrent environment adding to a vector is NOT thread-safe without locks, 
            // but updateCamera is typically called in the main thread (game loop).
            m_activeChunks.push_back(std::move(chunk));
        }
    }
}

} // namespace world
