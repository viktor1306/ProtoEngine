#pragma once

#include "assets/AssetTypes.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace proto {

// Portable model blobs are deliberately independent of the compiler's object
// layout.  All integer and floating point fields are written in little endian
// order and all section offsets are relative to the beginning of the blob.
// The fixed header is 136 bytes: magic[8], version/type/headerBytes (u32),
// payloadBytes (u64), sectionCount/reserved (u32), four reserved bytes, then
// six {offset:u64, length:u64} entries in model, nodes, meshes, pixel pool,
// textures, materials order.
inline constexpr uint32_t portableFormatVersion1 = 1;
inline constexpr uint32_t portableModelType = 1;
inline constexpr uint32_t portableHeaderBytes1 = 136;
inline constexpr uint32_t portableSectionCount1 = 6;
inline constexpr uint32_t portableTextureFormatRgba8 = 1;

std::vector<uint8_t> encodePortableModel(const ModelBundle& model);
std::shared_ptr<ModelBundle> decodePortableModel(std::span<const uint8_t> bytes);

} // namespace proto
