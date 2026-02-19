#include "World.hpp"
#include <iostream>

namespace world {

World::World(gfx::GeometryManager& geometryManager)
    : m_geometryManager(geometryManager)
{}

void World::generateTestWorld() {
    m_chunks.clear();
    m_meshes.clear();
    m_totalVertices = 0;
    m_totalIndices  = 0;

    // One test chunk at grid position (0,0,0)
    // fillTerrain uses seed=42 for deterministic terrain
    auto chunk = std::make_unique<Chunk>(0, 0, 0);
    chunk->fillTerrain(42);
    m_chunks.push_back(std::move(chunk));

    rebuildMeshes();
}

void World::rebuildMeshes() {
    m_meshes.clear();
    m_totalVertices = 0;
    m_totalIndices  = 0;

    for (const auto& chunk : m_chunks) {
        // No neighbours for legacy single-chunk world
        VoxelMeshData data = chunk->generateMesh();
        if (data.empty()) {
            std::cout << "[World] Chunk (" << chunk->getCX() << ","
                      << chunk->getCY() << "," << chunk->getCZ()
                      << ") produced empty mesh — skipping.\n";
            m_meshes.push_back(nullptr);
            continue;
        }

        // Upload via template method (VoxelVertex — 8 bytes)
        gfx::GeometryManager::UploadRequest req;
        gfx::Mesh* raw = m_geometryManager.allocateMeshRaw(static_cast<uint32_t>(data.vertices.size()), static_cast<uint32_t>(data.indices.size()), req, data.vertices, data.indices);
        m_geometryManager.executeBatchUpload({req});
        m_meshes.push_back(std::unique_ptr<gfx::Mesh>(raw));

        m_totalVertices += static_cast<uint32_t>(data.vertices.size());
        m_totalIndices  += static_cast<uint32_t>(data.indices.size());
    }

    std::cout << "[World] Rebuilt " << m_chunks.size() << " chunk(s). "
              << "Vertices: " << m_totalVertices
              << "  Indices: " << m_totalIndices << "\n";
}

void World::render(VkCommandBuffer cmd) {
    for (const auto& mesh : m_meshes) {
        if (mesh) mesh->draw(cmd);
    }
}

} // namespace world
