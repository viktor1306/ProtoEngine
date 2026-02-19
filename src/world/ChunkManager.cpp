#include "ChunkManager.hpp"
#include <chrono>
#include <iostream>
#include <cmath>
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
// submitMeshTask — enqueue one chunk meshing task with a specific LOD
// ---------------------------------------------------------------------------
void ChunkManager::submitMeshTask(const IVec3Key& key, int lod) {
    auto it = m_chunks.find(key);
    if (it == m_chunks.end()) return;

    std::array<const Chunk*, 6> neighbors = {
        getNeighbour(key.x + 1, key.y,     key.z    ),
        getNeighbour(key.x - 1, key.y,     key.z    ),
        getNeighbour(key.x,     key.y + 1, key.z    ),
        getNeighbour(key.x,     key.y - 1, key.z    ),
        getNeighbour(key.x,     key.y,     key.z + 1),
        getNeighbour(key.x,     key.y,     key.z - 1),
    };

    MeshTask task;
    task.chunk     = it->second.get();
    task.neighbors = neighbors;
    task.cx        = key.x;
    task.cy        = key.y;
    task.cz        = key.z;
    task.lod       = lod;
    m_meshWorker.submit(std::move(task));
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
    m_chunkLOD.clear();
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

    // Submit all chunks for async meshing with initial LOD based on current camera
    for (auto& [key, chunk] : m_chunks) {
        int lod = calculateLOD(key.x, key.y, key.z);
        m_chunkLOD[key] = lod;
        submitMeshTask(key, lod);
    }

    std::cout << "[ChunkManager] Submitted " << count
              << " meshing tasks to " << m_meshWorker.getThreadCount()
              << " worker threads.\n" << std::flush;
}

// ---------------------------------------------------------------------------
// calculateLOD — ring-based LOD selection by distance to camera
//
// Hysteresis logic:
//   Switching to a HIGHER LOD (coarser, farther): dist must exceed threshold + hysteresis
//   Switching to a LOWER  LOD (finer,  closer):   dist must be below  threshold - hysteresis
//   This prevents constant re-meshing when camera hovers near a boundary.
// ---------------------------------------------------------------------------
int ChunkManager::calculateLOD(int cx, int cy, int cz, int currentLOD) const {
    // Chunk center in world coords (no bias)
    const float half = static_cast<float>(CHUNK_SIZE) * 0.5f;
    float centerX = static_cast<float>(cx * CHUNK_SIZE) + half;
    float centerY = static_cast<float>(cy * CHUNK_SIZE) + half;
    float centerZ = static_cast<float>(cz * CHUNK_SIZE) + half;

    float dx = centerX - m_cameraPos.x;
    float dy = centerY - m_cameraPos.y;
    float dz = centerZ - m_cameraPos.z;
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    float d0 = std::max(0.0f, m_lodDist0);
    float d1 = std::max(d0,   m_lodDist1);
    float hy = std::max(0.0f, m_lodHysteresis);

    // Without hysteresis: simple threshold
    if (currentLOD < 0) {
        if (dist < d0) return 0;
        if (dist < d1) return 1;
        return 2;
    }

    // With hysteresis: apply epsilon based on direction of transition
    // Switching UP (to coarser LOD): need to go further past threshold
    // Switching DOWN (to finer LOD): need to come closer past threshold
    switch (currentLOD) {
        case 0:
            // Currently LOD 0: switch to 1 only if dist > d0 + hy
            if (dist > d0 + hy) return (dist > d1 + hy) ? 2 : 1;
            return 0;
        case 1:
            // Currently LOD 1: switch to 0 if dist < d0 - hy, to 2 if dist > d1 + hy
            if (dist < d0 - hy) return 0;
            if (dist > d1 + hy) return 2;
            return 1;
        case 2:
            // Currently LOD 2: switch to 1 only if dist < d1 - hy
            if (dist < d1 - hy) return (dist < d0 - hy) ? 0 : 1;
            return 2;
        default:
            // Unknown: use simple threshold
            if (dist < d0) return 0;
            if (dist < d1) return 1;
            return 2;
    }
}

// ---------------------------------------------------------------------------
// updateCamera — store camera position, mark chunks dirty on LOD changes,
//               and immediately flush dirty chunks to MeshWorker.
//
// Must be called BEFORE rebuildDirtyChunks() each frame.
// Hysteresis in calculateLOD() prevents flicker at LOD boundaries.
// ---------------------------------------------------------------------------
void ChunkManager::updateCamera(const core::math::Vec3& cameraPos) {
    m_cameraPos = cameraPos;

    for (const auto& [key, chunk] : m_chunks) {
        auto it = m_chunkLOD.find(key);
        int oldLOD = (it != m_chunkLOD.end()) ? it->second : -1;

        // Pass currentLOD so hysteresis can prevent boundary flicker
        int newLOD = calculateLOD(key.x, key.y, key.z, oldLOD);

        if (newLOD != oldLOD) {
            // Update LOD record BEFORE markDirty so flushDirty picks up the right LOD
            m_chunkLOD[key] = newLOD;
            markDirty(key.x, key.y, key.z);
        }
    }

    // Immediately submit all newly-dirty chunks to MeshWorker.
    // This ensures LOD transitions are processed without waiting for an
    // explicit flushDirty() call from the caller.
    flushDirty();
}

// ---------------------------------------------------------------------------
// getLODCounts — how many chunks are currently assigned to each LOD
// ---------------------------------------------------------------------------
std::array<uint32_t, 3> ChunkManager::getLODCounts() const {
    std::array<uint32_t, 3> out{0, 0, 0};
    for (const auto& [key, lod] : m_chunkLOD) {
        if (lod >= 0 && lod <= 2) out[static_cast<size_t>(lod)]++;
    }
    return out;
}

// ---------------------------------------------------------------------------
// worldToChunk — convert world voxel coord to chunk + local coord
// Handles negative coords correctly: wx=-1 → cx=-1, lx=31
// ---------------------------------------------------------------------------
static void worldToChunk(int wx, int CHUNK_SZ, int& cx, int& lx) {
    // floor division (works for negative numbers)
    cx = (wx >= 0) ? (wx / CHUNK_SZ) : ((wx - CHUNK_SZ + 1) / CHUNK_SZ);
    lx = wx - cx * CHUNK_SZ;
}

// ---------------------------------------------------------------------------
// getVoxel — read a voxel by world integer coords
// ---------------------------------------------------------------------------
VoxelData ChunkManager::getVoxel(int wx, int wy, int wz) const {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    auto it = m_chunks.find({cx, cy, cz});
    if (it == m_chunks.end()) return VOXEL_AIR;
    return it->second->getVoxel(lx, ly, lz);
}

// ---------------------------------------------------------------------------
// setVoxel — write a voxel by world integer coords + dirty neighbours
// ---------------------------------------------------------------------------
void ChunkManager::setVoxel(int wx, int wy, int wz, VoxelData v) {
    int cx, lx, cy, ly, cz, lz;
    worldToChunk(wx, CHUNK_SIZE, cx, lx);
    worldToChunk(wy, CHUNK_SIZE, cy, ly);
    worldToChunk(wz, CHUNK_SIZE, cz, lz);

    auto it = m_chunks.find({cx, cy, cz});
    if (it == m_chunks.end()) return; // outside loaded world

    it->second->setVoxel(lx, ly, lz, v);

    // Always dirty the owning chunk
    markDirty(cx, cy, cz);

    // Dirty boundary neighbours if the voxel is on a chunk edge
    // This ensures adjacent chunks re-mesh their exposed faces correctly.
    if (lx == 0)              markDirty(cx - 1, cy, cz);
    if (lx == CHUNK_SIZE - 1) markDirty(cx + 1, cy, cz);
    if (ly == 0)              markDirty(cx, cy - 1, cz);
    if (ly == CHUNK_SIZE - 1) markDirty(cx, cy + 1, cz);
    if (lz == 0)              markDirty(cx, cy, cz - 1);
    if (lz == CHUNK_SIZE - 1) markDirty(cx, cy, cz + 1);
}

// ---------------------------------------------------------------------------
// markDirty — only inserts into the dedup set; does NOT submit to MeshWorker.
// Call flushDirty() after all setVoxel calls to batch-submit unique chunks.
// ---------------------------------------------------------------------------
void ChunkManager::markDirty(int cx, int cy, int cz) {
    // Only mark chunks that actually exist
    if (m_chunks.find({cx, cy, cz}) == m_chunks.end()) return;
    m_dirtyPending.insert({cx, cy, cz});
}

// ---------------------------------------------------------------------------
// flushDirty — submit all pending dirty chunks to MeshWorker (deduplicated).
// ---------------------------------------------------------------------------
void ChunkManager::flushDirty() {
    for (const auto& key : m_dirtyPending) {
        auto it = m_chunks.find(key);
        if (it == m_chunks.end()) continue;
        it->second->markDirty();

        // Always submit with current desired LOD for this chunk
        int lod = calculateLOD(key.x, key.y, key.z);
        m_chunkLOD[key] = lod;
        submitMeshTask(key, lod);
    }
    m_dirtyPending.clear();
}

// ---------------------------------------------------------------------------
// rebuildDirtyChunks — collect async results and upload to GPU
//
// Використовує GeometryManager Sub-allocation (FreeList) та Batch Upload:
//   1. Збирає готові меші з MeshWorker (non-blocking).
//   2. Звільняє стару пам'ять мешу, виділяє нову.
//   3. Застосовує world bias прямо до локальних даних (які живуть до кінця функції).
//   4. Передає масив requests у GeometryManager для одного копіювання і одного MemoryBarrier.
//
// Це гарантує мінімальне блокування відеокарти та CPU.
// ---------------------------------------------------------------------------
void ChunkManager::rebuildDirtyChunks([[maybe_unused]] VkDevice device) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // Collect completed tasks (non-blocking)
    auto done = m_meshWorker.collect();
    if (done.empty()) return;

    std::cout << "[ChunkManager] Uploading " << done.size()
              << " chunk meshes to GPU...\n" << std::flush;

    std::vector<gfx::GeometryManager::UploadRequest> requests;
    requests.reserve(done.size());

    for (auto& task : done) {
        IVec3Key key{task.cx, task.cy, task.cz};

        // If this task's LOD is stale (camera moved), discard it.
        auto lodIt = m_chunkLOD.find(key);
        int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
        if (task.lod != desiredLOD) {
            continue;
        }

        auto& rd = m_renderData[key];

        // Free old mesh allocation
        if (rd.valid) {
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
            m_geometryManager.freeMesh(rd.mesh->getVertexOffset(), rd.mesh->getFirstIndex(), 
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex));
            rd.mesh.reset();
            rd.valid = false;
        }

        if (task.result.empty()) {
            if (task.lod == 0) {
                // chunk has no mesh (e.g. fully solid or air inside)
                auto it = m_chunks.find(key);
                if (it != m_chunks.end()) it->second->markClean();
            }
            continue; // Nothing to upload
        }

        // Apply world bias directly to task.result.vertices
        int worldOffX = key.x * CHUNK_SIZE + m_worldBiasX;
        int worldOffY = key.y * CHUNK_SIZE + m_worldBiasY;
        int worldOffZ = key.z * CHUNK_SIZE + m_worldBiasZ;

        for (VoxelVertex& v : task.result.vertices) {
            v.x = static_cast<uint8_t>(worldOffX + static_cast<int>(v.x));
            v.y = static_cast<uint8_t>(worldOffY + static_cast<int>(v.y));
            v.z = static_cast<uint8_t>(worldOffZ + static_cast<int>(v.z));
        }

        gfx::GeometryManager::UploadRequest req;
        rd.mesh.reset(m_geometryManager.allocateMeshRaw(static_cast<uint32_t>(task.result.vertices.size()), static_cast<uint32_t>(task.result.indices.size()), req, task.result.vertices, task.result.indices));
        rd.aabb = buildAABB(key.x, key.y, key.z);
        rd.vertexCount = static_cast<uint32_t>(task.result.vertices.size());
        rd.indexCount  = static_cast<uint32_t>(task.result.indices.size());
        rd.valid = true;

        m_totalVertices += rd.vertexCount;
        m_totalIndices  += rd.indexCount;

        requests.push_back(req);

        auto it = m_chunks.find(key);
        if (it != m_chunks.end()) it->second->markClean();
    }

    if (!requests.empty()) {
        m_geometryManager.executeBatchUpload(requests);
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
void ChunkManager::render(VkCommandBuffer cmd, [[maybe_unused]] const scene::Frustum& frustum) {
    m_visibleCount    = 0;
    m_culledCount     = 0;
    m_visibleVertices = 0;

    for (auto& [key, rd] : m_renderData) {
        if (!rd.valid || !rd.mesh) continue;

        // Frustum culling вимкнено — рендеримо всі чанки
        // (TODO: увімкнути після налаштування LOD дистанцій)

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
