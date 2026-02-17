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

    // One test chunk at world origin with a simple terrain layout:
    //   y 0..6  → STONE
    //   y 7     → DIRT
    //   y 8     → GRASS
    //   y 9..15 → AIR
    // Center the chunk around world origin: offset by -CHUNK_SIZE/2 on X and Z
    auto chunk = std::make_unique<Chunk>(core::math::Vec3{
        -static_cast<float>(CHUNK_SIZE) / 2.0f,
        0.0f,
        -static_cast<float>(CHUNK_SIZE) / 2.0f
    });
    chunk->fillTerrain(8); // groundY = 8
    m_chunks.push_back(std::move(chunk));

    rebuildMeshes();
}

void World::rebuildMeshes() {
    m_meshes.clear();
    m_totalVertices = 0;
    m_totalIndices  = 0;

    for (const auto& chunk : m_chunks) {
        MeshData data = chunk->generateMesh();
        if (data.empty()) {
            std::cout << "[World] Chunk at ("
                      << chunk->getWorldPos().x << ","
                      << chunk->getWorldPos().y << ","
                      << chunk->getWorldPos().z
                      << ") produced an empty mesh — skipping.\n";
            m_meshes.push_back(nullptr);
            continue;
        }

        // uploadMesh returns a raw Mesh* — wrap in unique_ptr for ownership
        gfx::Mesh* raw = m_geometryManager.uploadMesh(data.vertices, data.indices);
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
        if (mesh)
            mesh->draw(cmd);
    }
}

} // namespace world
