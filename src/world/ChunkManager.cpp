#include "ChunkManager.hpp"
#include <chrono>
#include <iostream>
#include <vulkan/vulkan.h>

namespace world {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ChunkManager::ChunkManager(gfx::GeometryManager& geometryManager,
                           uint32_t meshWorkerThreads)
    : m_geometryManager(geometryManager)
    , m_meshWorker(meshWorkerThreads)
{
    std::cout << "[ChunkManager] MeshWorker threads: "
              << m_meshWorker.getThreadCount() << "\n" << std::flush;
}

// ---------------------------------------------------------------------------
// getNeighbour
// ---------------------------------------------------------------------------
const Chunk* ChunkManager::getNeighbour(int cx, int cy, int cz) const {
    auto it = m_chunks.find({cx, cy, cz});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// buildAABB — world-space AABB for a chunk (true world coords, no bias)
// ---------------------------------------------------------------------------
scene::AABB ChunkManager::buildAABB(int cx, int cy, int cz) const {
    float wx = static_cast<float>(cx * CHUNK_SIZE);
    float wy = static_cast<float>(cy * CHUNK_SIZE);
    float wz = static_cast<float>(cz * CHUNK_SIZE);
    float sz = static_cast<float>(CHUNK_SIZE);
    return {
        {wx,      wy,      wz     },
        {wx + sz, wy + sz, wz + sz}
    };
}

// ---------------------------------------------------------------------------
// generateWorld — create terrain chunks and submit async meshing
// ---------------------------------------------------------------------------
void ChunkManager::generateWorld(int radiusX, int radiusZ, int seed) {
    // Wait for any in-flight meshing from previous world
    m_meshWorker.waitAll();
    m_meshWorker.collect(); // discard old results

    m_chunks.clear();
    m_renderData.clear();
    m_pendingMeshData.clear(); // скидаємо CPU кеш при повній регенерації
    m_totalVertices   = 0;
    m_totalIndices    = 0;
    m_visibleCount    = 0;
    m_culledCount     = 0;
    m_visibleVertices = 0;

    // Bias: shift all world coords so minimum = 0 (avoids uint8_t underflow)
    m_worldBiasX = radiusX * CHUNK_SIZE;
    m_worldBiasY = 0;
    m_worldBiasZ = radiusZ * CHUNK_SIZE;

    // Generate chunk data
    int count = 0;
    for (int cz = -radiusZ; cz <= radiusZ; ++cz) {
        for (int cx = -radiusX; cx <= radiusX; ++cx) {
            auto chunk = std::make_unique<Chunk>(cx, 0, cz);
            chunk->fillTerrain(seed);
            m_chunks[{cx, 0, cz}] = std::move(chunk);
            ++count;
        }
    }

    std::cout << "[ChunkManager] Generated " << count
              << " chunks (" << (2*radiusX+1) << "x" << (2*radiusZ+1) << " grid)."
              << " Bias=(" << m_worldBiasX << ",0," << m_worldBiasZ << ")\n" << std::flush;

    // Submit all chunks for async meshing
    for (auto& [key, chunk] : m_chunks) {
        std::array<const Chunk*, 6> neighbors = {
            getNeighbour(key.x + 1, key.y,     key.z    ),
            getNeighbour(key.x - 1, key.y,     key.z    ),
            getNeighbour(key.x,     key.y + 1, key.z    ),
            getNeighbour(key.x,     key.y - 1, key.z    ),
            getNeighbour(key.x,     key.y,     key.z + 1),
            getNeighbour(key.x,     key.y,     key.z - 1),
        };
        MeshTask task;
        task.chunk     = chunk.get();
        task.neighbors = neighbors;
        task.cx        = key.x;
        task.cy        = key.y;
        task.cz        = key.z;
        m_meshWorker.submit(std::move(task));
    }

    std::cout << "[ChunkManager] Submitted " << count
              << " meshing tasks to " << m_meshWorker.getThreadCount()
              << " worker threads.\n" << std::flush;
}

// ---------------------------------------------------------------------------
// markDirty
// ---------------------------------------------------------------------------
void ChunkManager::markDirty(int cx, int cy, int cz) {
    auto it = m_chunks.find({cx, cy, cz});
    if (it == m_chunks.end()) return;
    it->second->markDirty();

    // Re-submit for async meshing
    std::array<const Chunk*, 6> neighbors = {
        getNeighbour(cx + 1, cy,     cz    ),
        getNeighbour(cx - 1, cy,     cz    ),
        getNeighbour(cx,     cy + 1, cz    ),
        getNeighbour(cx,     cy - 1, cz    ),
        getNeighbour(cx,     cy,     cz + 1),
        getNeighbour(cx,     cy,     cz - 1),
    };
    MeshTask task;
    task.chunk     = it->second.get();
    task.neighbors = neighbors;
    task.cx        = cx;
    task.cy        = cy;
    task.cz        = cz;
    m_meshWorker.submit(std::move(task));
}

// ---------------------------------------------------------------------------
// uploadChunkMesh — upload one chunk's mesh to GPU (partial update)
// ---------------------------------------------------------------------------
void ChunkManager::uploadChunkMesh(const IVec3Key& key,
                                   VoxelMeshData& meshData,
                                   [[maybe_unused]] VkDevice device)
{
    // Apply world bias to vertex coords
    int worldOffX = key.x * CHUNK_SIZE + m_worldBiasX;
    int worldOffY = key.y * CHUNK_SIZE + m_worldBiasY;
    int worldOffZ = key.z * CHUNK_SIZE + m_worldBiasZ;

    for (VoxelVertex& v : meshData.vertices) {
        int wx = worldOffX + static_cast<int>(v.x);
        int wy = worldOffY + static_cast<int>(v.y);
        int wz = worldOffZ + static_cast<int>(v.z);
        v.x = static_cast<uint8_t>(wx);
        v.y = static_cast<uint8_t>(wy);
        v.z = static_cast<uint8_t>(wz);
    }

    // Get or create render data for this chunk
    auto& rd = m_renderData[key];

    // Destroy old mesh (if any) — partial update: only this chunk
    if (rd.valid) {
        // Update stats: subtract old counts
        m_totalVertices -= rd.vertexCount;
        m_totalIndices  -= rd.indexCount;
        rd.mesh.reset();
        rd.valid = false;
    }

    if (meshData.empty()) return;

    // Upload new mesh
    rd.mesh.reset(m_geometryManager.uploadMeshRaw(meshData.vertices, meshData.indices));
    rd.aabb        = buildAABB(key.x, key.y, key.z);
    rd.vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    rd.indexCount  = static_cast<uint32_t>(meshData.indices.size());
    rd.valid       = true;

    m_totalVertices += rd.vertexCount;
    m_totalIndices  += rd.indexCount;
}

// ---------------------------------------------------------------------------
// rebuildDirtyChunks — collect async results and upload to GPU
//
// GeometryManager використовує лінійний аллокатор (без звільнення окремих
// блоків). Тому при будь-якому оновленні ми:
//   1. Збираємо готові меші з MeshWorker (non-blocking).
//   2. Зберігаємо нові дані в m_pendingMeshData.
//   3. Робимо GeometryManager::reset() — скидаємо весь буфер.
//   4. Перезавантажуємо ВСІ валідні чанки (старі + нові).
//
// Це гарантує що буфер ніколи не переповниться.
// vkDeviceWaitIdle тільки якщо є що завантажувати.
// ---------------------------------------------------------------------------
void ChunkManager::rebuildDirtyChunks(VkDevice device) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // Collect completed tasks (non-blocking)
    auto done = m_meshWorker.collect();
    if (done.empty()) return;

    std::cout << "[ChunkManager] Uploading " << done.size()
              << " chunk meshes to GPU...\n" << std::flush;

    // Merge new results into pending cache
    for (auto& task : done) {
        IVec3Key key{task.cx, task.cy, task.cz};
        if (!task.result.empty()) {
            m_pendingMeshData[key] = std::move(task.result);
        } else {
            m_pendingMeshData.erase(key);
        }
        auto it = m_chunks.find(key);
        if (it != m_chunks.end()) it->second->markClean();
    }

    // GPU idle — safe to reset and re-upload
    vkDeviceWaitIdle(device);

    // Reset GeometryManager (linear allocator — full reset required)
    m_geometryManager.reset();
    m_renderData.clear();
    m_totalVertices = 0;
    m_totalIndices  = 0;

    // Re-upload ALL cached chunk meshes
    for (auto& [key, meshData] : m_pendingMeshData) {
        if (meshData.empty()) continue;

        // Apply world bias
        int worldOffX = key.x * CHUNK_SIZE + m_worldBiasX;
        int worldOffY = key.y * CHUNK_SIZE + m_worldBiasY;
        int worldOffZ = key.z * CHUNK_SIZE + m_worldBiasZ;

        // Copy and apply bias (meshData is our cache — don't modify in-place)
        std::vector<VoxelVertex> biasedVerts = meshData.vertices;
        for (VoxelVertex& v : biasedVerts) {
            v.x = static_cast<uint8_t>(worldOffX + static_cast<int>(v.x));
            v.y = static_cast<uint8_t>(worldOffY + static_cast<int>(v.y));
            v.z = static_cast<uint8_t>(worldOffZ + static_cast<int>(v.z));
        }

        auto& rd = m_renderData[key];
        rd.mesh.reset(m_geometryManager.uploadMeshRaw(biasedVerts, meshData.indices));
        rd.aabb        = buildAABB(key.x, key.y, key.z);
        rd.vertexCount = static_cast<uint32_t>(biasedVerts.size());
        rd.indexCount  = static_cast<uint32_t>(meshData.indices.size());
        rd.valid       = true;

        m_totalVertices += rd.vertexCount;
        m_totalIndices  += rd.indexCount;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastRebuildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    std::cout << "[ChunkManager] Upload done: "
              << m_totalVertices << " total vertices, "
              << m_totalIndices  << " total indices, "
              << m_lastRebuildMs << " ms\n" << std::flush;
}

// ---------------------------------------------------------------------------
// render — draw visible chunks with frustum culling
// ---------------------------------------------------------------------------
void ChunkManager::render(VkCommandBuffer cmd, const scene::Frustum& frustum) {
    m_visibleCount    = 0;
    m_culledCount     = 0;
    m_visibleVertices = 0;

    for (auto& [key, rd] : m_renderData) {
        if (!rd.valid || !rd.mesh) continue;

        // Frustum cull: skip chunks outside the view frustum
        if (!frustum.isVisible(rd.aabb)) {
            ++m_culledCount;
            continue;
        }

        rd.mesh->draw(cmd);
        ++m_visibleCount;
        m_visibleVertices += rd.vertexCount;
    }
}

// ---------------------------------------------------------------------------
// hasMesh
// ---------------------------------------------------------------------------
bool ChunkManager::hasMesh() const {
    for (auto& [key, rd] : m_renderData) {
        if (rd.valid && rd.mesh) return true;
    }
    return false;
}

} // namespace world
