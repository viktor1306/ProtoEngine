#include "ChunkManager.hpp"
#include <iostream>

namespace world {

ChunkManager::ChunkManager(gfx::GeometryManager& geometryManager, uint32_t meshWorkerThreads)
    : m_storage()
    , m_lodCtrl()
    , m_renderer(geometryManager, m_storage, m_lodCtrl, meshWorkerThreads)
{
    // The components initialize themselves.
}

void ChunkManager::generateWorld(int radiusX, int radiusZ, int seed) {
    m_renderer.clear();
    m_storage.generateWorld(radiusX, radiusZ, seed);

    auto& chunks = m_storage.getChunks();
    for (const auto& [key, chunk] : chunks) {
        int lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z);
        m_renderer.setLOD(key, lod);
        m_renderer.markDirty(key.x, key.y, key.z);
    }
    m_renderer.flushDirty();
}

void ChunkManager::rebuildDirtyChunks(VkDevice device) {
    m_renderer.rebuildDirtyChunks(device);
}

void ChunkManager::render(VkCommandBuffer cmd, const scene::Frustum& frustum) {
    m_renderer.render(cmd, frustum);
}

void ChunkManager::markDirty(int cx, int cy, int cz) {
    m_renderer.markDirty(cx, cy, cz);
}

void ChunkManager::flushDirty() {
    m_renderer.flushDirty();
}

VoxelData ChunkManager::getVoxel(int wx, int wy, int wz) const {
    return m_storage.getVoxel(wx, wy, wz);
}

void ChunkManager::setVoxel(int wx, int wy, int wz, VoxelData v) {
    m_storage.setVoxel(wx, wy, wz, v);

    // mark corresponding chunks dirty via renderer
    int cx = (wx >= 0) ? (wx / CHUNK_SIZE) : ((wx - CHUNK_SIZE + 1) / CHUNK_SIZE);
    int lx = wx - cx * CHUNK_SIZE;

    int cy = (wy >= 0) ? (wy / CHUNK_SIZE) : ((wy - CHUNK_SIZE + 1) / CHUNK_SIZE);
    int ly = wy - cy * CHUNK_SIZE;

    int cz = (wz >= 0) ? (wz / CHUNK_SIZE) : ((wz - CHUNK_SIZE + 1) / CHUNK_SIZE);
    int lz = wz - cz * CHUNK_SIZE;

    m_renderer.markDirty(cx, cy, cz);

    if (lx == 0)              m_renderer.markDirty(cx - 1, cy, cz);
    if (lx == CHUNK_SIZE - 1) m_renderer.markDirty(cx + 1, cy, cz);
    if (ly == 0)              m_renderer.markDirty(cx, cy - 1, cz);
    if (ly == CHUNK_SIZE - 1) m_renderer.markDirty(cx, cy + 1, cz);
    if (lz == 0)              m_renderer.markDirty(cx, cy, cz - 1);
    if (lz == CHUNK_SIZE - 1) m_renderer.markDirty(cx, cy, cz + 1);
}

void ChunkManager::updateCamera(const core::math::Vec3& cameraPos) {
    m_lodCtrl.setCameraPosition(cameraPos);

    for (const auto& [key, chunk] : m_storage.getChunks()) {
        int oldLOD = m_renderer.getLOD(key);
        int newLOD = m_lodCtrl.calculateLOD(key.x, key.y, key.z, oldLOD);

        if (newLOD != oldLOD) {
            m_renderer.setLOD(key, newLOD);
            m_renderer.markDirty(key.x, key.y, key.z);
        }
    }

    m_renderer.flushDirty();
}

int ChunkManager::calculateLOD(int cx, int cy, int cz, int currentLOD) const {
    return m_lodCtrl.calculateLOD(cx, cy, cz, currentLOD);
}

} // namespace world
