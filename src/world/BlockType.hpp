#pragma once

#include "core/Math.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace world {

// Unique identifier for a block type. 0 = AIR (always transparent/non-solid).
using BlockID = uint8_t;

// Static properties of a block type (shared across all instances).
struct BlockInfo {
    BlockID           id;
    std::string       name;
    bool              isSolid; // false = transparent (AIR, glass, etc.)
    core::math::Vec4  color;   // RGBA vertex color (texture atlas comes later)
};

// Global registry of all block types. Header-only singleton.
class BlockRegistry {
public:
    static void registerBlock(BlockInfo info) {
        s_blocks[info.id] = std::move(info);
    }

    static const BlockInfo& get(BlockID id) {
        auto it = s_blocks.find(id);
        if (it == s_blocks.end())
            throw std::runtime_error("BlockRegistry: unknown BlockID " + std::to_string(id));
        return it->second;
    }

    static bool isSolid(BlockID id) {
        auto it = s_blocks.find(id);
        return (it != s_blocks.end()) && it->second.isSolid;
    }

    // Register the built-in block types.
    static void registerDefaults() {
        registerBlock({0, "air",   false, {0.00f, 0.00f, 0.00f, 0.0f}});
        registerBlock({1, "stone", true,  {0.50f, 0.50f, 0.50f, 1.0f}});
        registerBlock({2, "grass", true,  {0.30f, 0.70f, 0.20f, 1.0f}});
        registerBlock({3, "dirt",  true,  {0.50f, 0.30f, 0.10f, 1.0f}});
        registerBlock({4, "sand",  true,  {0.85f, 0.80f, 0.55f, 1.0f}});
        registerBlock({5, "snow",  true,  {0.92f, 0.95f, 1.00f, 1.0f}});
        registerBlock({6, "water", true,  {0.10f, 0.35f, 0.80f, 1.0f}});
    }

    static const std::unordered_map<BlockID, BlockInfo>& all() { return s_blocks; }

private:
    // Inline static — C++17, no separate .cpp needed.
    inline static std::unordered_map<BlockID, BlockInfo> s_blocks;
};

} // namespace world
