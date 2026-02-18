#pragma once

#include "core/Math.hpp"
#include "world/VoxelData.hpp"
#include "world/ChunkManager.hpp"
#include <cmath>

namespace world {

// ---------------------------------------------------------------------------
// RayResult — result of a voxel raycast
// ---------------------------------------------------------------------------
struct RayResult {
    bool hit       = false;
    int  voxelX    = 0;   // world voxel coords of the hit voxel
    int  voxelY    = 0;
    int  voxelZ    = 0;
    int  normalX   = 0;   // face normal of the hit face (-1, 0, or +1)
    int  normalY   = 0;
    int  normalZ   = 0;
    float distance = 0.0f; // distance from ray origin to hit
};

// ---------------------------------------------------------------------------
// raycast — Amanatides-Woo DDA voxel traversal
//
// Traverses voxels along the ray (start + dir * t) up to maxDist.
// Returns the first solid voxel hit, or RayResult{hit=false} if none.
//
// Parameters:
//   cm       — ChunkManager to query voxels from
//   start    — ray origin in world space (float)
//   dir      — ray direction (does NOT need to be normalized, but should be)
//   maxDist  — maximum traversal distance
//
// World Bias note:
//   ChunkManager::getVoxel(wx, wy, wz) takes TRUE world integer coords.
//   We floor(start) to get the initial voxel, then step by ±1 per axis.
// ---------------------------------------------------------------------------
inline RayResult raycast(ChunkManager& cm,
                         core::math::Vec3 start,
                         core::math::Vec3 dir,
                         float maxDist)
{
    RayResult result{};

    // Normalize direction to avoid issues with very short/long vectors
    float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len < 1e-6f) return result;
    dir.x /= len;
    dir.y /= len;
    dir.z /= len;

    // Current voxel (integer coords)
    int x = static_cast<int>(std::floor(start.x));
    int y = static_cast<int>(std::floor(start.y));
    int z = static_cast<int>(std::floor(start.z));

    // Step direction per axis (+1 or -1)
    int stepX = (dir.x >= 0.0f) ? 1 : -1;
    int stepY = (dir.y >= 0.0f) ? 1 : -1;
    int stepZ = (dir.z >= 0.0f) ? 1 : -1;

    // tMax: distance to the next voxel boundary along each axis
    // tDelta: distance between consecutive boundaries along each axis
    float tMaxX, tMaxY, tMaxZ;
    float tDeltaX, tDeltaY, tDeltaZ;

    // Avoid division by zero for axis-aligned rays
    if (std::abs(dir.x) < 1e-9f) {
        tMaxX   = 1e30f;
        tDeltaX = 1e30f;
    } else {
        tDeltaX = std::abs(1.0f / dir.x);
        float boundX = (stepX > 0)
            ? (static_cast<float>(x + 1) - start.x)
            : (start.x - static_cast<float>(x));
        tMaxX = boundX * tDeltaX;
    }

    if (std::abs(dir.y) < 1e-9f) {
        tMaxY   = 1e30f;
        tDeltaY = 1e30f;
    } else {
        tDeltaY = std::abs(1.0f / dir.y);
        float boundY = (stepY > 0)
            ? (static_cast<float>(y + 1) - start.y)
            : (start.y - static_cast<float>(y));
        tMaxY = boundY * tDeltaY;
    }

    if (std::abs(dir.z) < 1e-9f) {
        tMaxZ   = 1e30f;
        tDeltaZ = 1e30f;
    } else {
        tDeltaZ = std::abs(1.0f / dir.z);
        float boundZ = (stepZ > 0)
            ? (static_cast<float>(z + 1) - start.z)
            : (start.z - static_cast<float>(z));
        tMaxZ = boundZ * tDeltaZ;
    }

    // Face normal of the last crossed boundary
    int nx = 0, ny = 0, nz = 0;

    // Traverse — DDA loop.
    // We step FIRST, then check the voxel we entered.
    // This avoids hitting the voxel the camera is currently inside.
    // Maximum iterations = maxDist / min(tDelta) + safety margin.
    const int maxSteps = static_cast<int>(maxDist * 3.0f) + 64;
    for (int step = 0; step < maxSteps; ++step) {
        // Step to next voxel boundary (smallest tMax)
        float t;
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            t      = tMaxX;
            x     += stepX;
            tMaxX += tDeltaX;
            nx = -stepX; ny = 0; nz = 0;
        } else if (tMaxY < tMaxZ) {
            t      = tMaxY;
            y     += stepY;
            tMaxY += tDeltaY;
            nx = 0; ny = -stepY; nz = 0;
        } else {
            t      = tMaxZ;
            z     += stepZ;
            tMaxZ += tDeltaZ;
            nx = 0; ny = 0; nz = -stepZ;
        }

        if (t > maxDist) break;

        // Check voxel we just entered
        VoxelData vox = cm.getVoxel(x, y, z);
        if (vox.isSolid()) {
            result.hit      = true;
            result.voxelX   = x;
            result.voxelY   = y;
            result.voxelZ   = z;
            result.normalX  = nx;
            result.normalY  = ny;
            result.normalZ  = nz;
            result.distance = t;
            return result;
        }
    }

    return result; // no hit
}

} // namespace world
