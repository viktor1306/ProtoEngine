#include "ChunkRenderer.hpp"
#include <chrono>
#include <iostream>

namespace world {

ChunkRenderer::ChunkRenderer(gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads)
    : m_geometryManager(geom), m_storage(storage), m_lodCtrl(lodCtrl), m_meshWorker(meshWorkerThreads)
{
    std::cout << "[ChunkRenderer] MeshWorker threads: "
              << m_meshWorker.getThreadCount() << "\n" << std::flush;
}

void ChunkRenderer::clear() {
    m_meshWorker.waitAll();
    m_meshWorker.collect();
    m_renderData.clear();
    m_chunkLOD.clear();
    m_dirtyPending.clear();
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_visibleCount = 0;
    m_culledCount = 0;
    m_visibleVertices = 0;
}

void ChunkRenderer::submitMeshTask(const IVec3Key& key, int lod) {
    auto chunk = m_storage.getChunk(key.x, key.y, key.z);
    if (!chunk) return;

    std::array<const Chunk*, 6> neighbors = {
        m_storage.getChunk(key.x + 1, key.y, key.z),
        m_storage.getChunk(key.x - 1, key.y, key.z),
        m_storage.getChunk(key.x, key.y + 1, key.z),
        m_storage.getChunk(key.x, key.y - 1, key.z),
        m_storage.getChunk(key.x, key.y, key.z + 1),
        m_storage.getChunk(key.x, key.y, key.z - 1)
    };

    MeshTask task;
    task.chunk = chunk;
    task.neighbors = neighbors;
    task.cx = key.x;
    task.cy = key.y;
    task.cz = key.z;
    task.lod = lod;
    m_meshWorker.submit(std::move(task));
}

scene::AABB ChunkRenderer::buildAABB(int cx, int cy, int cz) const {
    float wx = static_cast<float>(cx * CHUNK_SIZE);
    float wy = static_cast<float>(cy * CHUNK_SIZE);
    float wz = static_cast<float>(cz * CHUNK_SIZE);
    float sz = static_cast<float>(CHUNK_SIZE);
    return {{wx, wy, wz}, {wx + sz, wy + sz, wz + sz}};
}

void ChunkRenderer::markDirty(int cx, int cy, int cz) {
    if (!m_storage.getChunk(cx, cy, cz)) return;
    m_dirtyPending.insert({cx, cy, cz});
}

void ChunkRenderer::flushDirty() {
    for (const auto& key : m_dirtyPending) {
        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
        if (!chunk) continue;
        chunk->markDirty();

        int lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z);
        m_chunkLOD[key] = lod;
        submitMeshTask(key, lod);
    }
    m_dirtyPending.clear();
}

void ChunkRenderer::setLOD(const IVec3Key& key, int lod) {
    m_chunkLOD[key] = lod;
}

int ChunkRenderer::getLOD(const IVec3Key& key) const {
    auto it = m_chunkLOD.find(key);
    return (it != m_chunkLOD.end()) ? it->second : -1;
}

std::array<uint32_t, 3> ChunkRenderer::getLODCounts() const {
    std::array<uint32_t, 3> out{0, 0, 0};
    for (const auto& [key, lod] : m_chunkLOD) {
        if (lod >= 0 && lod <= 2) out[static_cast<size_t>(lod)]++;
    }
    return out;
}

bool ChunkRenderer::hasMesh() const {
    for (const auto& [key, rd] : m_renderData) {
        if (rd.valid && rd.mesh) return true;
    }
    return false;
}

void ChunkRenderer::render(VkCommandBuffer cmd, [[maybe_unused]] const scene::Frustum& frustum) {
    m_visibleCount    = 0;
    m_culledCount     = 0;
    m_visibleVertices = 0;

    for (const auto& [key, rd] : m_renderData) {
        if (!rd.valid || !rd.mesh) continue;

        rd.mesh->draw(cmd);
        ++m_visibleCount;
        m_visibleVertices += rd.vertexCount;
    }
}

void ChunkRenderer::rebuildDirtyChunks(VkDevice device) {
    auto t0 = std::chrono::high_resolution_clock::now();

    auto done = m_meshWorker.collect();
    if (done.empty()) return;

    std::unordered_map<IVec3Key, MeshTask, IVec3Hash> latestTasks;
    for (auto& task : done) {
        IVec3Key key{task.cx, task.cy, task.cz};
        auto lodIt = m_chunkLOD.find(key);
        int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
        if (task.lod == desiredLOD) {
            latestTasks[key] = std::move(task);
        }
    }

    if (!latestTasks.empty() && device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    std::vector<gfx::GeometryManager::UploadRequest> requests;
    requests.reserve(latestTasks.size());

    for (auto& [key, task] : latestTasks) {

        auto lodIt = m_chunkLOD.find(key);
        int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
        if (task.lod != desiredLOD) {
            continue;
        }

        auto& rd = m_renderData[key];

        if (rd.valid) {
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
            if (rd.mesh) {
                int32_t vOff = rd.mesh->getVertexOffset();
                uint32_t iOff = rd.mesh->getFirstIndex();
                m_geometryManager.freeMesh(vOff, iOff, 
                    rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex));
            }
            rd.mesh.reset();
            rd.valid = false;
        }

        if (task.result.empty()) {
            if (task.lod == 0) {
                auto chunk = m_storage.getChunk(key.x, key.y, key.z);
                if (chunk) chunk->markClean();
            }
            continue;
        }

        int worldOffX = key.x * CHUNK_SIZE + m_storage.getWorldBiasX();
        int worldOffY = key.y * CHUNK_SIZE + m_storage.getWorldBiasY();
        int worldOffZ = key.z * CHUNK_SIZE + m_storage.getWorldBiasZ();

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

        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
        if (chunk) chunk->markClean();
    }

    if (!requests.empty()) {
        m_geometryManager.executeBatchUpload(requests);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastRebuildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

} // namespace world
