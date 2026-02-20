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

    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunkPtr = chunks[i];
        if (!chunkPtr) continue;
        IVec3Key key{chunkPtr->getCX(), chunkPtr->getCY(), chunkPtr->getCZ()};
        int lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z);
        m_renderer.setLOD(key, lod);
        m_renderer.markDirty(key.x, key.y, key.z);
    }
    m_renderer.flushDirty();
}

void ChunkManager::rebuildDirtyChunks(VkDevice device, float currentTime) {
    m_renderer.rebuildDirtyChunks(device, currentTime);
}

void ChunkManager::render(VkCommandBuffer cmd, VkPipelineLayout layout, const scene::Frustum& frustum, float currentTime) {
    m_renderer.render(cmd, layout, frustum, currentTime);
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

    static core::math::Vec3 lastCameraPos = core::math::Vec3(999999.0f, 999999.0f, 999999.0f);
    float dx = cameraPos.x - lastCameraPos.x;
    float dy = cameraPos.y - lastCameraPos.y;
    float dz = cameraPos.z - lastCameraPos.z;
    if (std::sqrt(dx*dx + dy*dy + dz*dz) > 1.0f) {
        lastCameraPos = cameraPos;

        int dirtyCount = 0;
        // 1. Update LODs for existing chunks
        for (const auto& chunkPtr : m_storage.getChunks()) {
            if (!chunkPtr) continue;
            IVec3Key key{chunkPtr->getCX(), chunkPtr->getCY(), chunkPtr->getCZ()};
            int oldLOD = m_renderer.getLOD(key);
            int newLOD = m_lodCtrl.calculateLOD(key.x, key.y, key.z, oldLOD);

            if (newLOD != oldLOD) {
                m_renderer.setLOD(key, newLOD);
                m_renderer.markDirty(key.x, key.y, key.z);
                dirtyCount++;
            }
        }
        // if (dirtyCount > 0) {
        //     std::cout << "[ChunkManager] Camera Moved > 1.0f. Iterated chunks, marked " << dirtyCount << " chunks dirty." << std::endl;
        // }

        // 2. Demand-Driven Boost (Vertical Streaming)
        int playerWX = static_cast<int>(std::floor(cameraPos.x));
        int playerWY = static_cast<int>(std::floor(cameraPos.y));
        int playerWZ = static_cast<int>(std::floor(cameraPos.z));

        int px = (playerWX >= 0) ? (playerWX / CHUNK_SIZE) : ((playerWX - CHUNK_SIZE + 1) / CHUNK_SIZE);
        int pz = (playerWZ >= 0) ? (playerWZ / CHUNK_SIZE) : ((playerWZ - CHUNK_SIZE + 1) / CHUNK_SIZE);
        int py = (playerWY >= 0) ? (playerWY / CHUNK_SIZE) : ((playerWY - CHUNK_SIZE + 1) / CHUNK_SIZE);
        
        int localY = playerWY - py * CHUNK_SIZE;

        // Якщо ми близько до нижньої межі чанка (наприклад, менше 12 блоків)
        if (localY < 12) {
            int targetCY = py - 1;
            Chunk* underChunk = m_storage.getChunk(px, targetCY, pz);
            
            // Якщо чанк взагалі не існує (навіть не виділений), для простоти поки відкладемо його сторення до ChunkStorage.
            // Але якщо він є і ще UNGENERATED (або ми можемо попросити ChunkRenderer пропустити подію):
            if (underChunk && underChunk->m_state.load(std::memory_order_acquire) == ChunkState::UNGENERATED) {
                // Переводимо в GENERATING щоб не сабмітити мільйон тасок
                ChunkState expected = ChunkState::UNGENERATED;
                if (underChunk->m_state.compare_exchange_strong(expected, ChunkState::GENERATING, std::memory_order_acq_rel)) {
                    // Відправляємо Lock-Free Boost (GENERATE -> High)
                    m_renderer.submitGenerateTaskHigh(underChunk, 42); // Seed = 42 for now
                }
            } else if (!underChunk) {
                // Якщо його ще немає в grid'і – виділяємо пам'ять і відправляємо (тільки для CY >= m_minY)
                // Потребує модифікації m_storage для безпечного додавання.
                // Щоб уникнути переписування ChunkStorage під лок-фрі додавання, ми можемо додати `createChunkIfMissing` метод.
                m_storage.createChunkIfMissing(px, targetCY, pz, 42, m_renderer);
            }
        }
    }

    m_renderer.flushDirty();
}

int ChunkManager::calculateLOD(int cx, int cy, int cz, int currentLOD) const {
    return m_lodCtrl.calculateLOD(cx, cy, cz, currentLOD);
}

} // namespace world
