#include "ChunkStorage.hpp"
#include <iostream>

namespace world {

static void worldToChunk(int wx, int CHUNK_SZ, int& cx, int& lx) {
    cx = (wx >= 0) ? (wx / CHUNK_SZ) : ((wx - CHUNK_SZ + 1) / CHUNK_SZ);
    lx = wx - cx * CHUNK_SZ;
}

void ChunkStorage::generateWorld(int radiusX, int radiusZ, int seed) {
    m_chunks.clear();
    m_worldBiasX = radiusX * CHUNK_SIZE;
    m_worldBiasY = 0;
    m_worldBiasZ = radiusZ * CHUNK_SIZE;

    int count = 0;
    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            auto chunk = std::make_unique<Chunk>(cx, 0, cz);
            chunk->fillTerrain(seed);
            m_chunks[{cx, 0, cz}] = std::move(chunk);
            ++count;
        }
    }

    std::cout << "[ChunkStorage] Generated " << count
              << " chunks (" << (2*radiusX+1) << "x" << (2*radiusZ+1) << " grid)."
              << " Bias=(" << m_worldBiasX << ",0," << m_worldBiasZ << ")\n" << std::flush;
}

void ChunkStorage::clear() {
    m_chunks.clear();
}

VoxelData ChunkStorage::getVoxel(int wx, int wy, int wz) const {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    auto it = m_chunks.find({cx, cy, cz});
    if (it == m_chunks.end()) return VOXEL_AIR;
    return it->second->getVoxel(lx, ly, lz);
}

void ChunkStorage::setVoxel(int wx, int wy, int wz, VoxelData v) {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    auto it = m_chunks.find({cx, cy, cz});
    if (it == m_chunks.end()) return;
    it->second->setVoxel(lx, ly, lz, v);
}

const Chunk* ChunkStorage::getChunk(int cx, int cy, int cz) const {
    auto it = m_chunks.find({cx, cy, cz});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

Chunk* ChunkStorage::getChunk(int cx, int cy, int cz) {
    auto it = m_chunks.find({cx, cy, cz});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

} // namespace world
