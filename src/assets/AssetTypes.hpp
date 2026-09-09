#pragma once
#include "core/Id.hpp"
#include "core/Geometry.hpp"
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace proto {
struct AssetVertex {
    glm::vec3 position{}, normal{0, 1, 0};
    glm::vec4 tangent{1, 0, 0, 1}, color{1};
    glm::vec2 uv0{}, uv1{};
};
struct MeshPart {
    uint32_t firstIndex{}, indexCount{};
    AssetId material;
};
struct MeshTriangle {
    uint32_t firstIndex{}, part{};
};
struct MeshBvhNode {
    Bounds bounds;
    uint32_t first{}, count{}, left{}, right{};
};
struct MeshAsset {
    AssetId id;
    std::string name, revision;
    std::vector<AssetVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPart> parts;
    Bounds bounds;
    std::vector<MeshTriangle> triangles;
    std::vector<MeshBvhNode> bvh;
};
struct ImageMip {
    uint32_t width{}, height{};
    std::vector<uint8_t> rgba;
};
struct TexturePixels {
    std::string hash;
    bool srgb{};
    std::vector<ImageMip> mips;
};
struct TextureAsset {
    AssetId id;
    std::shared_ptr<const TexturePixels> pixels;
    int minFilter{9987}, magFilter{9729}, wrapS{10497}, wrapT{10497};
};
struct TextureSlot {
    AssetId texture;
    glm::vec2 offset{0}, scale{1};
    float rotation{};
    int texCoord{};
    bool operator==(const TextureSlot&) const = default;
};
struct MaterialValues {
    glm::vec4 baseColor{1};
    glm::vec3 emissive{0};
    float metallic{1}, roughness{1}, normalScale{1}, occlusion{1}, alphaCutoff{.5f};
    bool mask{}, doubleSided{}, unlit{};
    bool operator==(const MaterialValues&) const = default;
};
struct MaterialAsset {
    AssetId id, owner;
    std::string name;
    uint64_t revision{1};
    MaterialValues values;
    std::array<TextureSlot, 5> textures; // base, metallic/roughness, normal, occlusion, emissive
};
struct ModelNode {
    std::string name;
    int parent{-1};
    glm::mat4 local{1};
    AssetId mesh;
};
struct ModelBundle {
    AssetId id;
    std::string name, source, revision;
    std::vector<ModelNode> nodes;
    std::vector<std::shared_ptr<const MeshAsset>> meshes;
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    std::vector<std::shared_ptr<const MaterialAsset>> materials;
    std::vector<std::string> warnings;
    bool fromCache{};
    size_t decodedImages{};
};
struct AssetCatalog {
    std::unordered_map<AssetId, std::shared_ptr<const MeshAsset>> meshes;
    std::unordered_map<AssetId, std::shared_ptr<const TextureAsset>> textures;
    std::unordered_map<AssetId, std::shared_ptr<const MaterialAsset>> materials;
    std::unordered_map<AssetId, std::shared_ptr<const ModelBundle>> models;
    std::unordered_map<AssetId, std::shared_ptr<const std::vector<uint8_t>>> dataFiles;
    void publish(std::shared_ptr<const ModelBundle> model);
};
void validateMaterial(const MaterialValues& value);
void buildMeshBvh(MeshAsset& mesh);
std::optional<float> pickMesh(const MeshAsset& mesh, const AssetCatalog& assets, const std::vector<AssetId>& materials,
                              glm::vec3 origin, glm::vec3 direction, float maxDistance);
} // namespace proto
