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

    // Only immediately mesh chunks within the initial view radius.
    // Distant chunks have voxel data but no mesh yet — the streaming
    // system (updateCamera) will schedule them as the player approaches.
    // This prevents OOM when generating large worlds (e.g. radius 64).
    const float meshRadius = std::min(m_lodCtrl.m_lodDist1,
                                      static_cast<float>(radiusX * CHUNK_SIZE));

    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunkPtr = chunks[i];
        if (!chunkPtr) continue;
        IVec3Key key{chunkPtr->getCX(), chunkPtr->getCY(), chunkPtr->getCZ()};

        float cx_center = key.x * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
        float cz_center = key.z * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
        float distSq = cx_center * cx_center + cz_center * cz_center;

        if (distSq > meshRadius * meshRadius) continue; // let streaming handle it

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

void ChunkManager::updateCamera(const core::math::Vec3& cameraPos, const scene::Frustum& frustum) {
    m_lodCtrl.setCameraPosition(cameraPos);

    // --- STREAMING IN ---
    // Zone 1 (sphere): grid loop within m_unloadRadius — always load nearest columns
    {
        int playerWX = static_cast<int>(std::floor(cameraPos.x));
        int playerWZ = static_cast<int>(std::floor(cameraPos.z));
        int px = (playerWX >= 0) ? (playerWX / CHUNK_SIZE) : ((playerWX - CHUNK_SIZE + 1) / CHUNK_SIZE);
        int pz = (playerWZ >= 0) ? (playerWZ / CHUNK_SIZE) : ((playerWZ - CHUNK_SIZE + 1) / CHUNK_SIZE);

        const float sphereRadiusSq = m_unloadRadius * m_unloadRadius;
        int radius = static_cast<int>(std::ceil(m_unloadRadius / CHUNK_SIZE)) + 1;

        for (int cz = pz - radius; cz <= pz + radius; ++cz) {
            for (int cx = px - radius; cx <= px + radius; ++cx) {
                float cx_center = cx * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
                float cz_center = cz * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
                float dx_chunk = cameraPos.x - cx_center;
                float dz_chunk = cameraPos.z - cz_center;
                float distSq = dx_chunk * dx_chunk + dz_chunk * dz_chunk;

                if (distSq <= sphereRadiusSq) {
                    auto [minCY, maxCY] = m_storage.getSurfaceBounds(cx, cz, 42);
                    for (int cy = minCY; cy <= maxCY; ++cy)
                        m_storage.createChunkIfMissing(cx, cy, cz, 42, m_renderer);
                }
            }
        }
    }

    // Zone 2 (frustum): existing chunks with voxel data that need meshing.
    // createChunkIfMissing is a no-op for already-existing chunks, so we
    // use markDirty directly to schedule meshing for chunks inside the frustum.
    {
        const float sphereRadiusSq   = m_unloadRadius * m_unloadRadius;
        const float frustumMaxDistSq = m_frustumRadius * m_frustumRadius;

        for (const auto& chunkPtr : m_storage.getChunks()) {
            if (!chunkPtr) continue;
            IVec3Key key{chunkPtr->getCX(), chunkPtr->getCY(), chunkPtr->getCZ()};

            float cx_center = key.x * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
            float cz_center = key.z * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
            float dx_chunk  = cameraPos.x - cx_center;
            float dz_chunk  = cameraPos.z - cz_center;
            float distSq    = dx_chunk * dx_chunk + dz_chunk * dz_chunk;

            if (distSq <= sphereRadiusSq) continue;   // already in sphere (Zone 1)
            if (distSq >  frustumMaxDistSq) continue;  // too far

            float wy = static_cast<float>(key.y * CHUNK_SIZE);
            scene::AABB aabb{
                {cx_center - CHUNK_SIZE / 2.0f, wy,              cz_center - CHUNK_SIZE / 2.0f},
                {cx_center + CHUNK_SIZE / 2.0f, wy + CHUNK_SIZE, cz_center + CHUNK_SIZE / 2.0f}
            };
            if (frustum.isVisible(aabb)) {
                // Mark dirty so ChunkRenderer schedules mesh building for this chunk.
                // This triggers meshing for chunks that already have voxel data but no mesh.
                m_renderer.markDirty(key.x, key.y, key.z);
            }
        }
    }


    // --- STREAMING OUT + LOD UPDATE + FRUSTUM LOADING: one unified pass ---
    //
    // 3-tier architecture:
    //   tier 1: distSq <=  sphereR²            → keep voxels + GPU mesh
    //   tier 2: distSq <= frustumR² AND frustum → keep voxels + GPU mesh
    //   tier 3: distSq <= frustumR² NOT frustum → keep voxels, FREE GPU mesh
    //   tier 4: distSq >  frustumR²             → free voxels + GPU mesh
    //
    // Key insight: tier 3 keeps voxel data in storage so that when the camera
    // turns back (chunk re-enters frustum), Zone 2 can find and re-mesh it.
    {
        const float unloadRadiusSq   = m_unloadRadius * m_unloadRadius;
        const float frustumMaxDistSq = m_frustumRadius * m_frustumRadius;

        std::vector<IVec3Key> chunksToFullyRemove;  // voxels + GPU
        std::vector<IVec3Key> chunksMeshOnly;        // GPU mesh only (keep voxels)

        for (const auto& chunkPtr : m_storage.getChunks()) {
            if (!chunkPtr) continue;
            IVec3Key key{chunkPtr->getCX(), chunkPtr->getCY(), chunkPtr->getCZ()};

            float cx_center = key.x * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
            float cz_center = key.z * CHUNK_SIZE + CHUNK_SIZE / 2.0f;
            float dx_chunk  = cameraPos.x - cx_center;
            float dz_chunk  = cameraPos.z - cz_center;
            float distSq    = dx_chunk * dx_chunk + dz_chunk * dz_chunk;

            if (distSq > frustumMaxDistSq) {
                // Tier 4: too far — remove everything
                chunksToFullyRemove.push_back(key);
                continue;
            }

            // Compute AABB for frustum check (to be used by tier 2 & 3)
            float wy = static_cast<float>(key.y * CHUNK_SIZE);
            scene::AABB aabb{
                {cx_center - CHUNK_SIZE / 2.0f, wy,              cz_center - CHUNK_SIZE / 2.0f},
                {cx_center + CHUNK_SIZE / 2.0f, wy + CHUNK_SIZE, cz_center + CHUNK_SIZE / 2.0f}
            };

            bool inSphere  = (distSq <= unloadRadiusSq);
            bool inFrustum = frustum.isVisible(aabb);

            if (!inSphere && !inFrustum) {
                // Tier 3: beyond sphere and not in view — free GPU mesh, keep voxels
                // When camera turns back, chunk is still in m_storage.getChunks()
                // → the LOD update below will re-mesh it next frame.
                chunksMeshOnly.push_back(key);
                continue;
            }

            // Tier 1 or 2: keep chunk — update LOD / schedule meshing
            int oldLOD = m_renderer.getLOD(key);
            int newLOD = m_lodCtrl.calculateLOD(key.x, key.y, key.z, oldLOD);
            if (newLOD != oldLOD) {
                m_renderer.setLOD(key, newLOD);
                m_renderer.markDirty(key.x, key.y, key.z);
            } else if (oldLOD < 0) {
                // Voxels exist but no GPU mesh — assign LOD and mesh
                int lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z);
                m_renderer.setLOD(key, lod);
                m_renderer.markDirty(key.x, key.y, key.z);
            }
        }

        for (const auto& key : chunksToFullyRemove) {
            m_renderer.removeChunk(key);
            m_storage.removeChunk(key.x, key.y, key.z);
        }
        for (const auto& key : chunksMeshOnly) {
            m_renderer.removeChunk(key);  // free GPU only, voxels stay
        }


        // Vertical Streaming Boost
        int playerWX = static_cast<int>(std::floor(cameraPos.x));
        int playerWY = static_cast<int>(std::floor(cameraPos.y));
        int playerWZ = static_cast<int>(std::floor(cameraPos.z));
        int px = (playerWX >= 0) ? (playerWX / CHUNK_SIZE) : ((playerWX - CHUNK_SIZE + 1) / CHUNK_SIZE);
        int pz = (playerWZ >= 0) ? (playerWZ / CHUNK_SIZE) : ((playerWZ - CHUNK_SIZE + 1) / CHUNK_SIZE);
        int py = (playerWY >= 0) ? (playerWY / CHUNK_SIZE) : ((playerWY - CHUNK_SIZE + 1) / CHUNK_SIZE);

        int localY = playerWY - py * CHUNK_SIZE;
        if (localY < 12) {
            int targetCY = py - 1;
            Chunk* underChunk = m_storage.getChunk(px, targetCY, pz);
            if (underChunk && underChunk->m_state.load(std::memory_order_acquire) == ChunkState::UNGENERATED) {
                ChunkState expected = ChunkState::UNGENERATED;
                if (underChunk->m_state.compare_exchange_strong(expected, ChunkState::GENERATING, std::memory_order_acq_rel))
                    m_renderer.submitGenerateTaskHigh(underChunk, 42);
            } else if (!underChunk) {
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
