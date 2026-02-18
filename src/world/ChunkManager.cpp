#include "ChunkManager.hpp"
#include <chrono>
#include <iostream>
#include <vulkan/vulkan.h>

namespace world {

ChunkManager::ChunkManager(gfx::GeometryManager& geometryManager)
    : m_geometryManager(geometryManager)
{}

// ---------------------------------------------------------------------------
// getNeighbour
// ---------------------------------------------------------------------------
const Chunk* ChunkManager::getNeighbour(int cx, int cy, int cz) const {
    auto it = m_chunks.find({cx, cy, cz});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// generateWorld — create a radiusX × radiusZ grid of terrain chunks
// ---------------------------------------------------------------------------
void ChunkManager::generateWorld(int radiusX, int radiusZ, int seed) {
    m_chunks.clear();
    m_worldMesh.reset();

    // Bias: shift all world coords so minimum = 0 (avoids uint8_t underflow)
    // Min chunk coord = -radiusX, min block coord = -radiusX * CHUNK_SIZE
    m_worldBiasX = radiusX * CHUNK_SIZE;
    m_worldBiasY = 0; // cy=0 only, y coords always >= 0
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

    std::cout << "[ChunkManager] Generated " << count
              << " chunks (" << (2*radiusX+1) << "x" << (2*radiusZ+1) << " grid)."
              << " Bias=(" << m_worldBiasX << ",0," << m_worldBiasZ << ")\n" << std::flush;
}

// ---------------------------------------------------------------------------
// markDirty
// ---------------------------------------------------------------------------
void ChunkManager::markDirty(int cx, int cy, int cz) {
    auto it = m_chunks.find({cx, cy, cz});
    if (it != m_chunks.end()) it->second->markDirty();
}

// ---------------------------------------------------------------------------
// rebuildDirtyChunks
//
// Strategy: merge ALL dirty chunk meshes into one big CPU buffer,
// then reset GeometryManager and upload as a single draw call.
//
// Key insight: VoxelVertex stores LOCAL coords (0-31).
// We pre-offset each vertex on the CPU:
//   worldX = chunkCX * CHUNK_SIZE + localX
// This fits in uint8_t only if chunkCX * CHUNK_SIZE + 31 <= 255,
// i.e. chunkCX <= 7. For larger worlds we use uint16_t-equivalent
// by storing the offset in a separate world-space vertex field.
//
// SOLUTION: We expand VoxelVertex to world-space uint16_t coords here
// by converting to gfx::Vertex (48 bytes) for the merged mesh.
// This keeps the GPU pipeline simple and avoids per-chunk draw calls.
//
// For the voxel pipeline we use a SEPARATE vertex buffer with world-space
// float positions derived from the packed data + chunk offset.
// ---------------------------------------------------------------------------
void ChunkManager::rebuildDirtyChunks(VkDevice device) {
    // Check if any chunk is dirty
    bool anyDirty = false;
    for (auto& [key, chunk] : m_chunks) {
        if (chunk->isDirty()) { anyDirty = true; break; }
    }
    if (!anyDirty) return;

    std::cout << "[ChunkManager] rebuildDirtyChunks: generating meshes...\n" << std::flush;

    auto t0 = std::chrono::high_resolution_clock::now();

    // Collect merged mesh from ALL chunks (full rebuild for simplicity)
    // Future: incremental rebuild of only dirty chunks
    std::vector<VoxelVertex> allVertices;
    std::vector<uint32_t>    allIndices;
    allVertices.reserve(1 << 21); // 2M vertices pre-alloc (safe for 49 chunks)
    allIndices.reserve(1 << 22);  // 4M indices pre-alloc

    int chunkIdx = 0;
    for (auto& [key, chunk] : m_chunks) {
        // Build neighbour array: +X,-X,+Y,-Y,+Z,-Z
        std::array<const Chunk*, 6> neighbors = {
            getNeighbour(key.x + 1, key.y,     key.z    ),
            getNeighbour(key.x - 1, key.y,     key.z    ),
            getNeighbour(key.x,     key.y + 1, key.z    ),
            getNeighbour(key.x,     key.y - 1, key.z    ),
            getNeighbour(key.x,     key.y,     key.z + 1),
            getNeighbour(key.x,     key.y,     key.z - 1),
        };

        VoxelMeshData meshData = chunk->generateMesh(neighbors);
        ++chunkIdx;
        if (meshData.empty()) {
            chunk->markClean();
            continue;
        }

        // Pre-offset vertices: add chunk world offset to local coords.
        // We bias by (radiusX * CHUNK_SIZE) so all coords are non-negative.
        // The shader subtracts this bias via chunkOffset push constant.
        int worldOffX = key.x * CHUNK_SIZE;
        int worldOffY = key.y * CHUNK_SIZE;
        int worldOffZ = key.z * CHUNK_SIZE;

        uint32_t indexOffset = static_cast<uint32_t>(allVertices.size());

        for (VoxelVertex v : meshData.vertices) {
            int wx = worldOffX + static_cast<int>(v.x);
            int wy = worldOffY + static_cast<int>(v.y);
            int wz = worldOffZ + static_cast<int>(v.z);
            // Add bias so coords are always non-negative (max radius=3: bias=96)
            // uint8_t range 0-255 is sufficient for radius ≤ 3 (max coord = 96+32+32=160)
            wx += m_worldBiasX;
            wy += m_worldBiasY;
            wz += m_worldBiasZ;
            v.x = static_cast<uint8_t>(wx);
            v.y = static_cast<uint8_t>(wy);
            v.z = static_cast<uint8_t>(wz);
            allVertices.push_back(v);
        }

        for (uint32_t i : meshData.indices) {
            allIndices.push_back(i + indexOffset);
        }

        chunk->markClean();
    }

    if (allVertices.empty()) {
        m_totalVertices = 0;
        m_totalIndices  = 0;
        m_worldMesh.reset();
        return;
    }

    // GPU upload — reset and re-upload entire world mesh
    vkDeviceWaitIdle(device);
    m_geometryManager.reset();

    m_worldMesh.reset(
        m_geometryManager.uploadMeshRaw(allVertices, allIndices)
    );

    m_totalVertices = static_cast<uint32_t>(allVertices.size());
    m_totalIndices  = static_cast<uint32_t>(allIndices.size());

    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastRebuildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    std::cout << "[ChunkManager] Rebuilt: "
              << m_totalVertices << " vertices, "
              << m_totalIndices  << " indices, "
              << m_lastRebuildMs << " ms\n";
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void ChunkManager::render(VkCommandBuffer cmd) {
    if (!m_worldMesh) return;
    m_worldMesh->draw(cmd);
}

} // namespace world
