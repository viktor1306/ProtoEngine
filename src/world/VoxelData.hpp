#pragma once

#include <cstdint>
#include <array>
#include <vulkan/vulkan.h>

namespace world {

// ---------------------------------------------------------------------------
// VoxelData — 32-bit packed voxel storage
//
// Bit layout (LSB → MSB):
//   [11: 0]  palette_index  — 12 bits → 4096 block types
//   [19:12]  health         —  8 bits → 0-255 integrity
//   [23:20]  ao             —  4 bits → 16 AO levels
//   [31:24]  flags          —  8 bits → properties (see VoxelFlags)
// ---------------------------------------------------------------------------

enum VoxelFlags : uint8_t {
    VOXEL_FLAG_NONE        = 0,
    VOXEL_FLAG_SOLID       = 1 << 0,  // blocks movement
    VOXEL_FLAG_TRANSPARENT = 1 << 1,  // render neighbours through it
    VOXEL_FLAG_EMISSIVE    = 1 << 2,  // emits light
    VOXEL_FLAG_LIQUID      = 1 << 3,  // water / lava
    VOXEL_FLAG_FOLIAGE     = 1 << 4,  // double-sided rendering
};

struct VoxelData {
    uint32_t raw = 0;

    // ---- Constructors -------------------------------------------------------
    VoxelData() = default;
    explicit constexpr VoxelData(uint32_t r) : raw(r) {}

    // Factory: build from components
    static constexpr VoxelData make(uint16_t paletteIdx,
                                    uint8_t  health = 255,
                                    uint8_t  ao     = 0,
                                    uint8_t  flags  = VOXEL_FLAG_SOLID)
    {
        uint32_t r = 0;
        r |= static_cast<uint32_t>(paletteIdx & 0xFFFu);
        r |= static_cast<uint32_t>(health)              << 12;
        r |= static_cast<uint32_t>(ao & 0xFu)           << 20;
        r |= static_cast<uint32_t>(flags)               << 24;
        return VoxelData{r};
    }

    // ---- Accessors ----------------------------------------------------------
    [[nodiscard]] constexpr uint16_t getPaletteIndex() const { return static_cast<uint16_t>(raw & 0xFFFu); }
    [[nodiscard]] constexpr uint8_t  getHealth()       const { return static_cast<uint8_t>((raw >> 12) & 0xFFu); }
    [[nodiscard]] constexpr uint8_t  getAO()           const { return static_cast<uint8_t>((raw >> 20) & 0x0Fu); }
    [[nodiscard]] constexpr uint8_t  getFlags()        const { return static_cast<uint8_t>((raw >> 24) & 0xFFu); }

    // ---- Mutators -----------------------------------------------------------
    void setPaletteIndex(uint16_t idx) { raw = (raw & ~0xFFFu)        | static_cast<uint32_t>(idx & 0xFFFu); }
    void setHealth(uint8_t h)          { raw = (raw & ~(0xFFu << 12)) | (static_cast<uint32_t>(h)        << 12); }
    void setAO(uint8_t ao)             { raw = (raw & ~(0x0Fu << 20)) | (static_cast<uint32_t>(ao & 0xFu)<< 20); }
    void setFlags(uint8_t f)           { raw = (raw & ~(0xFFu << 24)) | (static_cast<uint32_t>(f)        << 24); }

    // ---- Queries ------------------------------------------------------------
    [[nodiscard]] constexpr bool isSolid()       const { return (getFlags() & VOXEL_FLAG_SOLID)       != 0; }
    [[nodiscard]] constexpr bool isTransparent() const { return (getFlags() & VOXEL_FLAG_TRANSPARENT) != 0; }
    [[nodiscard]] constexpr bool isEmissive()    const { return (getFlags() & VOXEL_FLAG_EMISSIVE)    != 0; }
    [[nodiscard]] constexpr bool isAir()         const { return raw == 0; }

    constexpr bool operator==(const VoxelData& o) const { return raw == o.raw; }
    constexpr bool operator!=(const VoxelData& o) const { return raw != o.raw; }
};

// AIR constant — raw = 0 (palette 0, no flags)
inline constexpr VoxelData VOXEL_AIR = VoxelData{0u};

// ---------------------------------------------------------------------------
// VoxelVertex — compressed vertex for the voxel pipeline
//
// Total size: 8 bytes (4-byte aligned via uint16_t tail)
//
// Layout:
//   byte 0   x          — local X in chunk (0-31)
//   byte 1   y          — local Y in chunk (0-31)
//   byte 2   z          — local Z in chunk (0-31)
//   byte 3   faceID     — 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
//   byte 4   ao         — ambient occlusion level (0-3)
//   byte 5   reserved   — future: light level
//   bytes 6-7 paletteIdx — block palette index (0-4095)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct VoxelVertex {
    uint8_t  x;
    uint8_t  y;
    uint8_t  z;
    uint8_t  faceID;
    uint8_t  ao;
    uint8_t  reserved;
    uint16_t paletteIdx;

    // Vulkan vertex input binding description
    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = sizeof(VoxelVertex); // 8 bytes
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // Vulkan vertex attribute descriptions
    // location 0: x,y,z,faceID  → VK_FORMAT_R8G8B8A8_UINT
    // location 1: ao,reserved,paletteIdx lo,paletteIdx hi → VK_FORMAT_R8G8B8A8_UINT
    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R8G8B8A8_UINT;
        attrs[0].offset   = 0; // x at byte 0

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R8G8B8A8_UINT;
        attrs[1].offset   = 4; // ao at byte 4

        return attrs;
    }
};
#pragma pack(pop)

static_assert(sizeof(VoxelVertex) == 8, "VoxelVertex must be 8 bytes");

// ---------------------------------------------------------------------------
// Face tables (indexed by faceID 0-5)
// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
// ---------------------------------------------------------------------------

// Normal vectors
inline constexpr float k_faceNormals[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

// Neighbour offsets (which adjacent voxel to check for culling)
inline constexpr int k_faceNeighbour[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

// 4 corner offsets per face (relative to voxel origin), for quad generation
// Each face: 4 vertices × 3 floats (dx, dy, dz)
inline constexpr float k_faceVerts[6][4][3] = {
    // +X
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
    // -X
    {{0,0,1},{0,1,1},{0,1,0},{0,0,0}},
    // +Y
    {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
    // -Y
    {{0,0,1},{0,0,0},{1,0,0},{1,0,1}},
    // +Z
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    // -Z
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
};

} // namespace world
