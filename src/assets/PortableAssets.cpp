#include "assets/PortableAssets.hpp"
#include "assets/AssetIO.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace proto {
namespace {

constexpr size_t kMaxBlobBytes = size_t(512) * 1024 * 1024;
constexpr uint32_t kMaxStringBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaxNodes = 100000;
constexpr uint32_t kMaxMeshes = 10000;
constexpr uint32_t kMaxVertices = 4000000;
constexpr uint32_t kMaxIndices = 12000000;
constexpr uint32_t kMaxParts = 1000000;
constexpr uint32_t kMaxTriangles = 4000000;
constexpr uint32_t kMaxBvhNodes = 8000000;
constexpr uint32_t kMaxPixels = 20000;
constexpr uint32_t kMaxMips = 14;
constexpr uint32_t kMaxTextures = 20000;
constexpr uint32_t kMaxMaterials = 4097;
constexpr uint32_t kMaxDimension = 8192;

constexpr uint32_t kSectionCount = portableSectionCount1;
constexpr uint32_t kHeaderBytes = portableHeaderBytes1;
constexpr uint32_t kModelSection = 0;
constexpr uint32_t kNodeSection = 1;
constexpr uint32_t kMeshSection = 2;
constexpr uint32_t kPixelSection = 3;
constexpr uint32_t kTextureSection = 4;
constexpr uint32_t kMaterialSection = 5;

constexpr std::array<uint8_t, 8> kMagic{'P', 'r', 'o', 't', 'o', 'M', '6', 0};

struct SectionRange {
    uint64_t offset{};
    uint64_t length{};
};

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

bool finite(const glm::vec2& value) { return std::isfinite(value.x) && std::isfinite(value.y); }
bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool finite(const glm::vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
bool finite(const glm::mat4& value) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(value[column][row]))
                return false;
    return true;
}
bool finite(const Bounds& value) { return finite(value.min) && finite(value.max); }

void validateBounds(const Bounds& bounds) {
    if (!finite(bounds) || bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z)
        invalid("Portable model has invalid bounds");
}

bool validMinFilter(int value) {
    return value == 9728 || value == 9729 || value == 9984 || value == 9985 || value == 9986 || value == 9987;
}
bool validMagFilter(int value) { return value == 9728 || value == 9729; }
bool validWrap(int value) { return value == 33071 || value == 33648 || value == 10497; }

void validateId(const AssetId& id, const char* what) {
    if (!id)
        throw std::runtime_error(std::string("Portable model has empty ") + what + " ID");
}

template <class T> void validateUniqueIds(const std::vector<std::shared_ptr<const T>>& values, const char* what) {
    std::unordered_set<AssetId> ids;
    ids.reserve(values.size());
    for (const auto& value : values) {
        if (!value)
            throw std::runtime_error(std::string("Portable model has null ") + what);
        validateId(value->id, what);
        if (!ids.insert(value->id).second)
            throw std::runtime_error(std::string("Portable model has duplicate ") + what + " ID");
    }
}

void validateString(const std::string& value, const char* what) {
    if (value.size() > kMaxStringBytes)
        throw std::runtime_error(std::string("Portable model ") + what + " is too long");
}

void validateModel(const ModelBundle& model) {
    validateId(model.id, "model");
    validateString(model.name, "name");
    validateString(model.revision, "revision");
    if (model.nodes.size() > kMaxNodes || model.meshes.size() > kMaxMeshes || model.textures.size() > kMaxTextures ||
        model.materials.size() > kMaxMaterials)
        invalid("Portable model exceeds resource count limit");

    validateUniqueIds(model.meshes, "mesh");
    validateUniqueIds(model.textures, "texture");
    validateUniqueIds(model.materials, "material");

    std::unordered_set<AssetId> allResourceIds;
    allResourceIds.reserve(1 + model.meshes.size() + model.textures.size() + model.materials.size());
    allResourceIds.insert(model.id);
    const auto checkGlobalIds = [&allResourceIds](const auto& values, const char* what) {
        for (const auto& value : values)
            if (!allResourceIds.insert(value->id).second)
                throw std::runtime_error(std::string("Portable model has duplicate global ") + what + " ID");
    };
    checkGlobalIds(model.meshes, "resource");
    checkGlobalIds(model.textures, "resource");
    checkGlobalIds(model.materials, "resource");

    std::unordered_set<AssetId> meshIds;
    meshIds.reserve(model.meshes.size());
    for (const auto& mesh : model.meshes)
        meshIds.insert(mesh->id);
    std::unordered_set<AssetId> textureIds;
    textureIds.reserve(model.textures.size());
    for (const auto& texture : model.textures)
        textureIds.insert(texture->id);
    std::unordered_set<AssetId> materialIds;
    materialIds.reserve(model.materials.size());
    for (const auto& material : model.materials)
        materialIds.insert(material->id);

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& node = model.nodes[i];
        validateString(node.name, "node name");
        if (node.parent < -1 || node.parent >= static_cast<int>(i))
            invalid("Portable model has invalid node parent");
        if (!finite(node.local))
            invalid("Portable model has non-finite node matrix");
        if (node.mesh && !meshIds.contains(node.mesh))
            invalid("Portable model node references an unknown mesh");
    }

    for (const auto& mesh : model.meshes) {
        validateString(mesh->name, "mesh name");
        validateString(mesh->revision, "mesh revision");
        validateBounds(mesh->bounds);
        if (mesh->vertices.size() > kMaxVertices || mesh->indices.size() > kMaxIndices ||
            mesh->parts.size() > kMaxParts || mesh->triangles.size() > kMaxTriangles || mesh->bvh.size() > kMaxBvhNodes)
            invalid("Portable mesh exceeds geometry count limit");
        for (const auto& vertex : mesh->vertices) {
            if (!finite(vertex.position) || !finite(vertex.normal) || !finite(vertex.tangent) || !finite(vertex.color) ||
                !finite(vertex.uv0) || !finite(vertex.uv1))
                invalid("Portable mesh has non-finite vertex data");
        }
        for (const auto index : mesh->indices)
            if (index >= mesh->vertices.size())
                invalid("Portable mesh index is outside the vertex buffer");
        for (const auto& part : mesh->parts) {
            if (part.firstIndex > mesh->indices.size() || part.indexCount > mesh->indices.size() - part.firstIndex ||
                !part.indexCount || part.indexCount % 3)
                invalid("Portable mesh has invalid part range");
            if (!part.material || !materialIds.contains(part.material))
                invalid("Portable mesh part references an unknown material");
        }
        for (const auto& triangle : mesh->triangles) {
            if (triangle.firstIndex > mesh->indices.size() || 3 > mesh->indices.size() - triangle.firstIndex ||
                triangle.firstIndex % 3 || triangle.part >= mesh->parts.size())
                invalid("Portable mesh has invalid triangle range");
            const auto& part = mesh->parts[triangle.part];
            if (triangle.firstIndex < part.firstIndex || triangle.firstIndex - part.firstIndex > part.indexCount - 3)
                invalid("Portable triangle is outside its mesh part");
        }
        for (const auto& node : mesh->bvh) {
            validateBounds(node.bounds);
            if (node.count) {
                if (node.first > mesh->triangles.size() || node.count > mesh->triangles.size() - node.first)
                    invalid("Portable mesh has invalid BVH leaf range");
            } else if (node.left >= mesh->bvh.size() || node.right >= mesh->bvh.size())
                invalid("Portable mesh has invalid BVH child");
        }
        if (!mesh->bvh.empty()) {
            // BVH nodes form a rooted tree.  Enter/exit states catch back edges,
            // while the parent table rejects shared children and repeated roots.
            std::vector<uint8_t> state(mesh->bvh.size());
            std::vector<uint32_t> parent(mesh->bvh.size(), UINT32_MAX);
            parent[0] = 0;
            struct Visit {
                uint32_t index;
                bool exit;
            };
            std::vector<Visit> stack{{0, false}};
            while (!stack.empty()) {
                const auto visit = stack.back();
                stack.pop_back();
                if (visit.index >= mesh->bvh.size())
                    invalid("Portable mesh BVH child is out of range");
                if (visit.exit) {
                    if (state[visit.index] != 1)
                        invalid("Portable mesh BVH has an invalid exit");
                    state[visit.index] = 2;
                    continue;
                }
                if (state[visit.index] != 0)
                    invalid("Portable mesh BVH is cyclic or shared");
                state[visit.index] = 1;
                stack.push_back({visit.index, true});
                const auto& node = mesh->bvh[visit.index];
                if (!node.count) {
                    if (node.left == node.right)
                        invalid("Portable mesh BVH repeats a child");
                    for (const auto child : {node.right, node.left}) {
                        if (child >= mesh->bvh.size() || parent[child] != UINT32_MAX)
                            invalid("Portable mesh BVH has a repeated child");
                        parent[child] = visit.index;
                        stack.push_back({child, false});
                    }
                }
            }
            for (const auto nodeState : state)
                if (nodeState != 2)
                    invalid("Portable mesh BVH has an unreachable node");
        }
    }

    std::unordered_map<const TexturePixels*, uint32_t> pixelPointers;
    pixelPointers.reserve(model.textures.size());
    for (const auto& texture : model.textures) {
        if (!texture->pixels)
            invalid("Portable texture has no pixel data");
        if (!validMinFilter(texture->minFilter) || !validMagFilter(texture->magFilter) || !validWrap(texture->wrapS) ||
            !validWrap(texture->wrapT))
            invalid("Portable texture has invalid sampler enum");
        pixelPointers.try_emplace(texture->pixels.get(), static_cast<uint32_t>(pixelPointers.size()));
    }
    uint64_t totalPixelBytes{};
    for (const auto& [pixels, index] : pixelPointers) {
        (void)index;
        validateString(pixels->hash, "texture hash");
        if (pixels->mips.empty() || pixels->mips.size() > kMaxMips)
            invalid("Portable texture has invalid mip count");
        uint32_t expectedWidth{}, expectedHeight{};
        for (size_t i = 0; i < pixels->mips.size(); ++i) {
            const auto& mip = pixels->mips[i];
            if (!mip.width || !mip.height || mip.width > kMaxDimension || mip.height > kMaxDimension)
                invalid("Portable texture has invalid mip dimensions");
            const uint64_t expectedBytes = uint64_t(mip.width) * uint64_t(mip.height) * 4;
            if (expectedBytes > kMaxBlobBytes || mip.rgba.size() != expectedBytes)
                invalid("Portable texture has invalid mip pixels");
            totalPixelBytes += expectedBytes;
            if (totalPixelBytes > kMaxBlobBytes)
                invalid("Portable texture pixels exceed blob budget");
            if (!i) {
                expectedWidth = mip.width;
                expectedHeight = mip.height;
            } else {
                expectedWidth = std::max(1u, expectedWidth / 2);
                expectedHeight = std::max(1u, expectedHeight / 2);
                if (mip.width != expectedWidth || mip.height != expectedHeight)
                    invalid("Portable texture mip chain is not contiguous");
            }
        }
    }
    for (const auto& material : model.materials) {
        validateString(material->name, "material name");
        validateId(material->owner, "material owner");
        if (material->owner != model.id)
            invalid("Portable material owner does not match its model");
        try {
            validateMaterial(material->values);
        } catch (const std::exception&) {
            throw std::runtime_error("Portable material has invalid values");
        }
        if (!finite(material->values.baseColor) || !finite(material->values.emissive))
            invalid("Portable material has non-finite values");
        for (const auto& slot : material->textures) {
            if (!finite(slot.offset) || !finite(slot.scale) || !std::isfinite(slot.rotation) || slot.texCoord < 0 ||
                slot.texCoord > 1)
                invalid("Portable material has invalid texture transform");
            if (slot.texture && !textureIds.contains(slot.texture))
                invalid("Portable material references an unknown texture");
        }
    }
}

struct Writer {
    std::vector<uint8_t> bytes;

    void reserve(size_t count) { bytes.reserve(count); }
    void u8(uint8_t value) { bytes.push_back(value); }
    void u32(uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }
    void i32(int32_t value) { u32(std::bit_cast<uint32_t>(value)); }
    void u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
    void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }
    void id(const AssetId& value) { bytes.insert(bytes.end(), value.uuid.bytes.begin(), value.uuid.bytes.end()); }
    void string(const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max())
            invalid("Portable string is too long");
        u32(static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void vec2(const glm::vec2& value) {
        f32(value.x);
        f32(value.y);
    }
    void vec3(const glm::vec3& value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }
    void vec4(const glm::vec4& value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
        f32(value.w);
    }
    void bounds(const Bounds& value) {
        vec3(value.min);
        vec3(value.max);
    }
    void matrix(const glm::mat4& value) {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                f32(value[column][row]);
    }
};

constexpr std::string_view kPortablePixelHashPrefix = "proto-portable-image-v1:";

Writer canonicalPixels(const TexturePixels& pixels) {
    Writer writer;
    writer.u32(portableTextureFormatRgba8);
    writer.u8(pixels.srgb ? 1 : 0);
    writer.u32(static_cast<uint32_t>(pixels.mips.size()));
    for (const auto& mip : pixels.mips) {
        writer.u32(mip.width);
        writer.u32(mip.height);
        writer.u64(static_cast<uint64_t>(mip.rgba.size()));
        writer.bytes.insert(writer.bytes.end(), mip.rgba.begin(), mip.rgba.end());
    }
    return writer;
}

std::string portablePixelHash(const TexturePixels& pixels) {
    const auto canonical = canonicalPixels(pixels);
    return std::string(kPortablePixelHashPrefix) + sha256(std::span<const uint8_t>(canonical.bytes.data(), canonical.bytes.size()));
}

void patchU32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    if (at > bytes.size() || bytes.size() - at < 4)
        invalid("Portable header patch outside blob");
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[at + shift / 8] = static_cast<uint8_t>(value >> shift);
}
void patchU64(std::vector<uint8_t>& bytes, size_t at, uint64_t value) {
    if (at > bytes.size() || bytes.size() - at < 8)
        invalid("Portable header patch outside blob");
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes[at + shift / 8] = static_cast<uint8_t>(value >> shift);
}

struct Reader {
    std::span<const uint8_t> bytes;
    size_t at{};
    size_t end{};

    size_t remaining() const { return end - at; }
    void require(size_t count) const {
        if (count > end - at)
            invalid("Truncated portable model");
    }
    uint8_t u8() {
        require(1);
        return bytes[at++];
    }
    uint32_t u32() {
        require(4);
        const uint32_t value = uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
                               (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
        at += 4;
        return value;
    }
    int32_t i32() { return std::bit_cast<int32_t>(u32()); }
    uint64_t u64() {
        require(8);
        uint64_t value{};
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= uint64_t(bytes[at + shift / 8]) << shift;
        at += 8;
        return value;
    }
    float f32() {
        const auto value = std::bit_cast<float>(u32());
        if (!std::isfinite(value))
            invalid("Portable model has non-finite number");
        return value;
    }
    AssetId id() {
        require(16);
        AssetId value;
        std::copy_n(bytes.data() + at, 16, value.uuid.bytes.data());
        at += 16;
        return value;
    }
    std::string string() {
        const auto count = u32();
        if (count > kMaxStringBytes || count > remaining())
            invalid("Invalid portable string");
        std::string value(reinterpret_cast<const char*>(bytes.data() + at), count);
        at += count;
        return value;
    }
    glm::vec2 vec2() { return {f32(), f32()}; }
    glm::vec3 vec3() { return {f32(), f32(), f32()}; }
    glm::vec4 vec4() { return {f32(), f32(), f32(), f32()}; }
    Bounds bounds() { return {vec3(), vec3()}; }
    glm::mat4 matrix() {
        glm::mat4 value(1);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                value[column][row] = f32();
        return value;
    }
    uint32_t count(uint32_t maximum, size_t minimumBytesPerItem) {
        const auto value = u32();
        if (value > maximum || (minimumBytesPerItem && value > remaining() / minimumBytesPerItem))
            invalid("Portable model count exceeds section");
        return value;
    }
};

Writer makeModelSection(const ModelBundle& model) {
    Writer writer;
    writer.id(model.id);
    writer.string(model.name);
    writer.string(model.revision);
    return writer;
}

Writer makeNodeSection(const ModelBundle& model) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(model.nodes.size()));
    for (const auto& node : model.nodes) {
        writer.string(node.name);
        writer.i32(node.parent);
        writer.matrix(node.local);
        writer.id(node.mesh);
    }
    return writer;
}

Writer makeMeshSection(const ModelBundle& model) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(model.meshes.size()));
    for (const auto& mesh : model.meshes) {
        writer.id(mesh->id);
        writer.string(mesh->name);
        writer.string(mesh->revision);
        writer.bounds(mesh->bounds);
        writer.u32(static_cast<uint32_t>(mesh->vertices.size()));
        for (const auto& vertex : mesh->vertices) {
            writer.vec3(vertex.position);
            writer.vec3(vertex.normal);
            writer.vec4(vertex.tangent);
            writer.vec4(vertex.color);
            writer.vec2(vertex.uv0);
            writer.vec2(vertex.uv1);
        }
        writer.u32(static_cast<uint32_t>(mesh->indices.size()));
        for (const auto index : mesh->indices)
            writer.u32(index);
        writer.u32(static_cast<uint32_t>(mesh->parts.size()));
        for (const auto& part : mesh->parts) {
            writer.u32(part.firstIndex);
            writer.u32(part.indexCount);
            writer.id(part.material);
        }
        writer.u32(static_cast<uint32_t>(mesh->triangles.size()));
        for (const auto& triangle : mesh->triangles) {
            writer.u32(triangle.firstIndex);
            writer.u32(triangle.part);
        }
        writer.u32(static_cast<uint32_t>(mesh->bvh.size()));
        for (const auto& node : mesh->bvh) {
            writer.bounds(node.bounds);
            writer.u32(node.first);
            writer.u32(node.count);
            writer.u32(node.left);
            writer.u32(node.right);
        }
    }
    return writer;
}

Writer makePixelSection(const ModelBundle& model, std::vector<std::shared_ptr<const TexturePixels>>& pixels) {
    std::unordered_map<const TexturePixels*, uint32_t> indices;
    for (const auto& texture : model.textures)
        if (const auto [it, inserted] = indices.try_emplace(texture->pixels.get(), static_cast<uint32_t>(pixels.size()));
            inserted)
            pixels.push_back(texture->pixels);

    Writer writer;
    writer.u32(static_cast<uint32_t>(pixels.size()));
    for (const auto& pixel : pixels) {
        writer.string(portablePixelHash(*pixel));
        const auto canonical = canonicalPixels(*pixel);
        writer.bytes.insert(writer.bytes.end(), canonical.bytes.begin(), canonical.bytes.end());
    }
    return writer;
}

Writer makeTextureSection(const ModelBundle& model, const std::vector<std::shared_ptr<const TexturePixels>>& pixels) {
    std::unordered_map<const TexturePixels*, uint32_t> indices;
    indices.reserve(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i)
        indices.emplace(pixels[i].get(), static_cast<uint32_t>(i));

    Writer writer;
    writer.u32(static_cast<uint32_t>(model.textures.size()));
    for (const auto& texture : model.textures) {
        writer.id(texture->id);
        writer.u32(indices.at(texture->pixels.get()));
        writer.i32(texture->minFilter);
        writer.i32(texture->magFilter);
        writer.i32(texture->wrapS);
        writer.i32(texture->wrapT);
    }
    return writer;
}

void writeMaterialValues(Writer& writer, const MaterialValues& values) {
    writer.vec4(values.baseColor);
    writer.vec3(values.emissive);
    writer.f32(values.metallic);
    writer.f32(values.roughness);
    writer.f32(values.normalScale);
    writer.f32(values.occlusion);
    writer.f32(values.alphaCutoff);
    writer.u8(values.mask ? 1 : 0);
    writer.u8(values.doubleSided ? 1 : 0);
    writer.u8(values.unlit ? 1 : 0);
}

Writer makeMaterialSection(const ModelBundle& model) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(model.materials.size()));
    for (const auto& material : model.materials) {
        writer.id(material->id);
        writer.id(material->owner);
        writer.string(material->name);
        writer.u64(material->revision);
        writeMaterialValues(writer, material->values);
        for (const auto& slot : material->textures) {
            writer.id(slot.texture);
            writer.vec2(slot.offset);
            writer.vec2(slot.scale);
            writer.f32(slot.rotation);
            writer.i32(slot.texCoord);
        }
    }
    return writer;
}

void readMaterialValues(Reader& reader, MaterialValues& values) {
    values.baseColor = reader.vec4();
    values.emissive = reader.vec3();
    values.metallic = reader.f32();
    values.roughness = reader.f32();
    values.normalScale = reader.f32();
    values.occlusion = reader.f32();
    values.alphaCutoff = reader.f32();
    const auto mask = reader.u8(), doubleSided = reader.u8(), unlit = reader.u8();
    if (mask > 1 || doubleSided > 1 || unlit > 1)
        invalid("Portable material has invalid flags");
    values.mask = mask != 0;
    values.doubleSided = doubleSided != 0;
    values.unlit = unlit != 0;
}

std::shared_ptr<ModelBundle> readPayload(std::span<const uint8_t> bytes, const std::array<SectionRange, kSectionCount>& sections) {
    auto model = std::make_shared<ModelBundle>();

    {
        Reader reader{bytes, static_cast<size_t>(sections[kModelSection].offset),
                      static_cast<size_t>(sections[kModelSection].offset + sections[kModelSection].length)};
        model->id = reader.id();
        model->name = reader.string();
        model->revision = reader.string();
        if (reader.at != reader.end)
            invalid("Trailing portable model fields");
    }
    {
        Reader reader{bytes, static_cast<size_t>(sections[kNodeSection].offset),
                      static_cast<size_t>(sections[kNodeSection].offset + sections[kNodeSection].length)};
        const auto count = reader.count(kMaxNodes, 88);
        model->nodes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ModelNode node;
            node.name = reader.string();
            node.parent = reader.i32();
            node.local = reader.matrix();
            node.mesh = reader.id();
            model->nodes.push_back(std::move(node));
        }
        if (reader.at != reader.end)
            invalid("Trailing portable node fields");
    }
    {
        Reader reader{bytes, static_cast<size_t>(sections[kMeshSection].offset),
                      static_cast<size_t>(sections[kMeshSection].offset + sections[kMeshSection].length)};
        const auto count = reader.count(kMaxMeshes, 68);
        model->meshes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto mesh = std::make_shared<MeshAsset>();
            mesh->id = reader.id();
            mesh->name = reader.string();
            mesh->revision = reader.string();
            mesh->bounds = reader.bounds();
            const auto vertexCount = reader.count(kMaxVertices, 72);
            mesh->vertices.reserve(vertexCount);
            for (uint32_t v = 0; v < vertexCount; ++v) {
                AssetVertex vertex;
                vertex.position = reader.vec3();
                vertex.normal = reader.vec3();
                vertex.tangent = reader.vec4();
                vertex.color = reader.vec4();
                vertex.uv0 = reader.vec2();
                vertex.uv1 = reader.vec2();
                mesh->vertices.push_back(vertex);
            }
            const auto indexCount = reader.count(kMaxIndices, 4);
            mesh->indices.reserve(indexCount);
            for (uint32_t index = 0; index < indexCount; ++index)
                mesh->indices.push_back(reader.u32());
            const auto partCount = reader.count(kMaxParts, 24);
            mesh->parts.reserve(partCount);
            for (uint32_t part = 0; part < partCount; ++part)
                mesh->parts.push_back({reader.u32(), reader.u32(), reader.id()});
            const auto triangleCount = reader.count(kMaxTriangles, 8);
            mesh->triangles.reserve(triangleCount);
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
                mesh->triangles.push_back({reader.u32(), reader.u32()});
            const auto bvhCount = reader.count(kMaxBvhNodes, 40);
            mesh->bvh.reserve(bvhCount);
            for (uint32_t node = 0; node < bvhCount; ++node) {
                MeshBvhNode value;
                value.bounds = reader.bounds();
                value.first = reader.u32();
                value.count = reader.u32();
                value.left = reader.u32();
                value.right = reader.u32();
                mesh->bvh.push_back(value);
            }
            model->meshes.push_back(std::move(mesh));
        }
        if (reader.at != reader.end)
            invalid("Trailing portable mesh fields");
    }

    std::vector<std::shared_ptr<TexturePixels>> pixels;
    std::vector<uint32_t> pixelAliases;
    std::unordered_map<std::string, uint32_t> canonicalPixelIndices;
    {
        Reader reader{bytes, static_cast<size_t>(sections[kPixelSection].offset),
                      static_cast<size_t>(sections[kPixelSection].offset + sections[kPixelSection].length)};
        const auto count = reader.count(kMaxPixels, 13);
        pixels.reserve(count);
        pixelAliases.reserve(count);
        canonicalPixelIndices.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto pixel = std::make_shared<TexturePixels>();
            const auto ignoredSerializedHash = reader.string();
            (void)ignoredSerializedHash;
            if (reader.u32() != portableTextureFormatRgba8)
                invalid("Portable texture format is unsupported");
            const auto srgb = reader.u8();
            if (srgb > 1)
                invalid("Portable texture has invalid color space flag");
            pixel->srgb = srgb != 0;
            const auto mipCount = reader.count(kMaxMips, 16);
            pixel->mips.reserve(mipCount);
            for (uint32_t mipIndex = 0; mipIndex < mipCount; ++mipIndex) {
                ImageMip mip;
                mip.width = reader.u32();
                mip.height = reader.u32();
                const auto byteCount = reader.u64();
                if (!mip.width || !mip.height || mip.width > kMaxDimension || mip.height > kMaxDimension ||
                    byteCount != uint64_t(mip.width) * uint64_t(mip.height) * 4 || byteCount > kMaxBlobBytes ||
                    byteCount > reader.remaining())
                    invalid("Portable texture has invalid mip payload");
                mip.rgba.resize(static_cast<size_t>(byteCount));
                std::copy_n(bytes.data() + reader.at, mip.rgba.size(), mip.rgba.data());
                reader.at += mip.rgba.size();
                pixel->mips.push_back(std::move(mip));
            }
            pixel->hash = portablePixelHash(*pixel);
            const auto [it, inserted] = canonicalPixelIndices.try_emplace(pixel->hash, static_cast<uint32_t>(pixels.size()));
            if (inserted)
                pixels.push_back(std::move(pixel));
            pixelAliases.push_back(it->second);
        }
        if (reader.at != reader.end)
            invalid("Trailing portable pixel fields");
    }
    {
        Reader reader{bytes, static_cast<size_t>(sections[kTextureSection].offset),
                      static_cast<size_t>(sections[kTextureSection].offset + sections[kTextureSection].length)};
        const auto count = reader.count(kMaxTextures, 36);
        model->textures.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto texture = std::make_shared<TextureAsset>();
            texture->id = reader.id();
            const auto pixelIndex = reader.u32();
            if (pixelIndex >= pixelAliases.size())
                invalid("Portable texture references an unknown pixel pool entry");
            texture->pixels = pixels[pixelAliases[pixelIndex]];
            texture->minFilter = reader.i32();
            texture->magFilter = reader.i32();
            texture->wrapS = reader.i32();
            texture->wrapT = reader.i32();
            model->textures.push_back(std::move(texture));
        }
        if (reader.at != reader.end)
            invalid("Trailing portable texture fields");
    }
    {
        Reader reader{bytes, static_cast<size_t>(sections[kMaterialSection].offset),
                      static_cast<size_t>(sections[kMaterialSection].offset + sections[kMaterialSection].length)};
        const auto count = reader.count(kMaxMaterials, 295);
        model->materials.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto material = std::make_shared<MaterialAsset>();
            material->id = reader.id();
            material->owner = reader.id();
            material->name = reader.string();
            material->revision = reader.u64();
            readMaterialValues(reader, material->values);
            for (auto& slot : material->textures) {
                slot.texture = reader.id();
                slot.offset = reader.vec2();
                slot.scale = reader.vec2();
                slot.rotation = reader.f32();
                slot.texCoord = reader.i32();
            }
            model->materials.push_back(std::move(material));
        }
        if (reader.at != reader.end)
            invalid("Trailing portable material fields");
    }
    model->fromCache = true;
    model->decodedImages = 0;
    validateModel(*model);
    return model;
}

} // namespace

std::vector<uint8_t> encodePortableModel(const ModelBundle& model) {
    validateModel(model);
    std::vector<std::shared_ptr<const TexturePixels>> pixels;
    std::array<Writer, kSectionCount> sections{makeModelSection(model), makeNodeSection(model), makeMeshSection(model),
                                               makePixelSection(model, pixels), makeTextureSection(model, pixels),
                                               makeMaterialSection(model)};

    size_t totalBytes = kHeaderBytes;
    for (const auto& section : sections) {
        if (section.bytes.size() > kMaxBlobBytes - totalBytes)
            invalid("Portable model exceeds blob budget");
        totalBytes += section.bytes.size();
    }
    if (totalBytes > kMaxBlobBytes || totalBytes < kHeaderBytes)
        invalid("Portable model exceeds blob budget");

    std::vector<uint8_t> blob(kHeaderBytes);
    std::copy(kMagic.begin(), kMagic.end(), blob.begin());
    patchU32(blob, 8, portableFormatVersion1);
    patchU32(blob, 12, portableModelType);
    patchU32(blob, 16, kHeaderBytes);
    patchU64(blob, 20, static_cast<uint64_t>(totalBytes - kHeaderBytes));
    patchU32(blob, 28, kSectionCount);
    patchU32(blob, 32, 0);
    size_t offset = kHeaderBytes;
    for (size_t i = 0; i < sections.size(); ++i) {
        blob.insert(blob.end(), sections[i].bytes.begin(), sections[i].bytes.end());
        patchU64(blob, 40 + i * 16, static_cast<uint64_t>(offset));
        patchU64(blob, 48 + i * 16, static_cast<uint64_t>(sections[i].bytes.size()));
        offset += sections[i].bytes.size();
    }
    return blob;
}

std::shared_ptr<ModelBundle> decodePortableModel(std::span<const uint8_t> bytes) {
    if (bytes.size() > kMaxBlobBytes || bytes.size() < kHeaderBytes)
        invalid("Portable model blob size is invalid");
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        invalid("Portable model magic mismatch");

    const auto readAtU32 = [&](size_t at) {
        if (at > bytes.size() || bytes.size() - at < 4)
            invalid("Truncated portable model header");
        return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) |
               (uint32_t(bytes[at + 3]) << 24);
    };
    const auto readAtU64 = [&](size_t at) {
        if (at > bytes.size() || bytes.size() - at < 8)
            invalid("Truncated portable model header");
        uint64_t value{};
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= uint64_t(bytes[at + shift / 8]) << shift;
        return value;
    };
    if (readAtU32(8) != portableFormatVersion1 || readAtU32(12) != portableModelType || readAtU32(16) != kHeaderBytes ||
        readAtU32(28) != kSectionCount || readAtU32(32) != 0 || readAtU32(36) != 0)
        invalid("Portable model header mismatch");
    const auto payloadBytes = readAtU64(20);
    if (payloadBytes != bytes.size() - kHeaderBytes)
        invalid("Portable model payload size mismatch");

    std::array<SectionRange, kSectionCount> sections{};
    uint64_t previousEnd = kHeaderBytes;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto offset = readAtU64(40 + i * 16), length = readAtU64(48 + i * 16);
        if (offset < kHeaderBytes || offset > bytes.size() || length > bytes.size() - offset || offset != previousEnd)
            invalid("Portable model section range is invalid");
        sections[i] = {offset, length};
        previousEnd = offset + length;
    }
    if (previousEnd != bytes.size())
        invalid("Portable model sections do not cover payload");

    try {
        return readPayload(bytes, sections);
    } catch (const std::out_of_range&) {
        invalid("Portable model references an out-of-range value");
    }
}

} // namespace proto
