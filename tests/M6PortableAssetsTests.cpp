#include "assets/PortableAssets.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace proto;

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void rejects(const std::function<void()>& action, const char* label = "") {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error(std::string("Expected portable asset rejection: ") + label);
}

AssetId id(unsigned index) {
    std::string value = "00000000-0000-4000-8000-000000000000";
    const char* digits = "0123456789abcdef";
    value[value.size() - 1] = digits[index & 15];
    value[value.size() - 2] = digits[(index >> 4) & 15];
    return AssetId::parse(value);
}

ModelBundle fixture() {
    ModelBundle model;
    model.id = id(1);
    model.name = "shared-pixel-fixture";
    model.source = "Editor/author/path/should-not-ship.glb";
    model.revision = "model-revision";
    model.warnings = {"editor-only warning"};

    auto pixels = std::make_shared<TexturePixels>();
    pixels->hash = "shared-pixels";
    pixels->srgb = true;
    pixels->mips = {{2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}},
                    {1, 1, {128, 128, 128, 255}}};

    auto texture0 = std::make_shared<TextureAsset>();
    texture0->id = id(2);
    texture0->pixels = pixels;
    texture0->minFilter = 9987;
    texture0->magFilter = 9729;
    texture0->wrapS = 10497;
    texture0->wrapT = 33071;
    auto texture1 = std::make_shared<TextureAsset>(*texture0);
    texture1->id = id(3);
    model.textures = {texture0, texture1};

    auto material = std::make_shared<MaterialAsset>();
    material->id = id(4);
    material->owner = model.id;
    material->name = "mask-material";
    material->revision = 7;
    material->values.mask = true;
    material->values.alphaCutoff = .25f;
    material->values.baseColor = {.8f, .7f, .6f, .5f};
    material->textures[0].texture = texture0->id;
    material->textures[0].offset = {.1f, .2f};
    material->textures[0].scale = {2.f, 3.f};
    material->textures[0].rotation = .125f;
    material->textures[0].texCoord = 1;
    model.materials.push_back(material);

    auto mesh = std::make_shared<MeshAsset>();
    mesh->id = id(5);
    mesh->name = "triangle";
    mesh->revision = "mesh-revision";
    mesh->vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 0}, {0, 0}},
                      {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {1, 0}, {1, 0}},
                      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {0, 1}, {0, 1}}};
    mesh->indices = {0, 1, 2};
    mesh->parts = {{0, 3, material->id}};
    mesh->bounds = {{0, 0, 0}, {1, 1, 0}};
    mesh->triangles = {{0, 0}};
    mesh->bvh = {{{{0, 0, 0}, {1, 1, 0}}, 0, 1, 0, 0}};
    model.meshes.push_back(mesh);

    model.nodes.push_back({"root", -1, glm::mat4(1), mesh->id});
    auto child = glm::mat4(1);
    child[3][0] = 2.f;
    model.nodes.push_back({"child", 0, child, mesh->id});
    return model;
}

ModelBundle bvhTreeFixture() {
    auto model = fixture();
    auto mesh = std::make_shared<MeshAsset>(*model.meshes[0]);
    const auto bounds = mesh->bounds;
    mesh->bvh = {{bounds, 0, 0, 1, 4},
                 {bounds, 0, 0, 2, 3},
                 {bounds, 0, 1, 0, 0},
                 {bounds, 0, 1, 0, 0},
                 {bounds, 0, 1, 0, 0}};
    model.meshes[0] = mesh;
    return model;
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) | (uint32_t(bytes.at(at + 2)) << 16) |
           (uint32_t(bytes.at(at + 3)) << 24);
}

uint64_t readU64(const std::vector<uint8_t>& bytes, size_t at) {
    uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= uint64_t(bytes.at(at + shift / 8)) << shift;
    return value;
}

void writeU32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.at(at + shift / 8) = static_cast<uint8_t>(value >> shift);
}

size_t skipString(const std::vector<uint8_t>& bytes, size_t at) {
    return at + 4 + readU32(bytes, at);
}

size_t meshBvhNodeOffset(const std::vector<uint8_t>& bytes, uint32_t node) {
    size_t at = static_cast<size_t>(readU64(bytes, 72)) + 4; // mesh count
    at += 16;                                                  // mesh ID
    at = skipString(bytes, at);                                // mesh name
    at = skipString(bytes, at);                                // mesh revision
    at += 24;                                                   // bounds
    const auto vertices = readU32(bytes, at);
    at += 4 + size_t(vertices) * 72;
    const auto indices = readU32(bytes, at);
    at += 4 + size_t(indices) * 4;
    const auto parts = readU32(bytes, at);
    at += 4 + size_t(parts) * 24;
    const auto triangles = readU32(bytes, at);
    at += 4 + size_t(triangles) * 8;
    check(node < readU32(bytes, at), "BVH fixture node exists");
    return at + 4 + size_t(node) * 40;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    try {
        const auto original = fixture();
        const auto encoded = encodePortableModel(original);
        check(encoded.size() > portableHeaderBytes1, "Portable fixture encoded");
        check(readU32(encoded, 8) == portableFormatVersion1 && readU32(encoded, 12) == portableModelType,
              "Portable header version/type");
        const std::string authoredPath = original.source;
        check(std::search(encoded.begin(), encoded.end(), authoredPath.begin(), authoredPath.end()) == encoded.end(),
              "Authored source path is not serialized");

        const auto decoded = decodePortableModel(encoded);
        check(decoded->id == original.id && decoded->name == original.name && decoded->revision == original.revision,
              "Model identity roundtrip");
        check(decoded->source.empty() && decoded->warnings.empty() && decoded->fromCache && decoded->decodedImages == 0,
              "Editor-only model data stripped");
        check(decoded->nodes.size() == 2 && decoded->nodes[1].parent == 0 && decoded->nodes[1].local[3][0] == 2.f,
              "Node hierarchy and matrices roundtrip");
        check(decoded->meshes.size() == 1 && decoded->meshes[0]->vertices.size() == 3 &&
                  decoded->meshes[0]->vertices[1].position == original.meshes[0]->vertices[1].position &&
                  decoded->meshes[0]->vertices[1].tangent == original.meshes[0]->vertices[1].tangent &&
                  decoded->meshes[0]->indices == original.meshes[0]->indices &&
                  decoded->meshes[0]->parts[0].material == original.meshes[0]->parts[0].material &&
                  decoded->meshes[0]->bvh.size() == 1 && decoded->meshes[0]->bvh[0].first == 0 &&
                  decoded->meshes[0]->bvh[0].count == 1,
              "Mesh geometry and BVH roundtrip");
        check(decoded->textures.size() == 2 && decoded->textures[0]->pixels == decoded->textures[1]->pixels &&
                  decoded->textures[0]->pixels->mips.size() == 2 &&
                  decoded->textures[0]->pixels->mips[0].rgba == original.textures[0]->pixels->mips[0].rgba &&
                  decoded->textures[0]->pixels->mips[1].width == 1 &&
                  decoded->textures[0]->pixels->hash.rfind("proto-portable-image-v1:", 0) == 0,
              "Shared texture pixels and mips roundtrip");
        check(decoded->materials.size() == 1 && decoded->materials[0]->revision == 7 &&
                  decoded->materials[0]->textures[0] == original.materials[0]->textures[0],
              "Material values and texture slots roundtrip");
        check(encodePortableModel(*decoded) == encoded, "Portable encoding is deterministic");

        auto truncated = encoded;
        truncated.pop_back();
        rejects([&] { decodePortableModel(truncated); }, "truncated");
        auto badMagic = encoded;
        badMagic[0] ^= 1;
        rejects([&] { decodePortableModel(badMagic); }, "bad magic");
        auto badPayload = encoded;
        writeU32(badPayload, 20, readU32(badPayload, 20) + 1);
        rejects([&] { decodePortableModel(badPayload); }, "bad payload");
        auto badSection = encoded;
        writeU32(badSection, 40, 0);
        rejects([&] { decodePortableModel(badSection); }, "bad section");
        auto oversizedCount = encoded;
        const auto nodeSection = static_cast<size_t>(readU64(oversizedCount, 56));
        writeU32(oversizedCount, nodeSection, 0xffffffffu);
        rejects([&] { decodePortableModel(oversizedCount); }, "oversized count");

        auto treeEncoded = encodePortableModel(bvhTreeFixture());
        auto selfLoop = treeEncoded;
        writeU32(selfLoop, meshBvhNodeOffset(selfLoop, 1) + 32, 1);
        rejects([&] { decodePortableModel(selfLoop); }, "BVH self-loop");
        auto twoNodeCycle = treeEncoded;
        writeU32(twoNodeCycle, meshBvhNodeOffset(twoNodeCycle, 1) + 32, 0);
        rejects([&] { decodePortableModel(twoNodeCycle); }, "BVH two-node cycle");
        auto repeatedChild = treeEncoded;
        writeU32(repeatedChild, meshBvhNodeOffset(repeatedChild, 0) + 36, 1);
        rejects([&] { decodePortableModel(repeatedChild); }, "BVH repeated child");

        auto crossTypeId = fixture();
        auto collidingTexture = std::make_shared<TextureAsset>(*crossTypeId.textures[0]);
        collidingTexture->id = crossTypeId.meshes[0]->id;
        crossTypeId.textures[0] = collidingTexture;
        rejects([&] { encodePortableModel(crossTypeId); }, "cross-type resource ID collision");
        auto wrongOwner = fixture();
        auto ownerMaterial = std::make_shared<MaterialAsset>(*wrongOwner.materials[0]);
        ownerMaterial->owner = id(99);
        wrongOwner.materials[0] = ownerMaterial;
        rejects([&] { encodePortableModel(wrongOwner); }, "material owner mismatch");

        auto conflictingPixels = fixture();
        auto alteredPixels = std::make_shared<TexturePixels>(*conflictingPixels.textures[1]->pixels);
        alteredPixels->hash = "builtin:white";
        alteredPixels->mips[0].rgba[0] ^= 1;
        auto alteredTexture = std::make_shared<TextureAsset>(*conflictingPixels.textures[1]);
        alteredTexture->pixels = alteredPixels;
        conflictingPixels.textures[1] = alteredTexture;
        const auto conflictingDecoded = decodePortableModel(encodePortableModel(conflictingPixels));
        check(conflictingDecoded->textures[0]->pixels != conflictingDecoded->textures[1]->pixels &&
                  conflictingDecoded->textures[0]->pixels->hash != conflictingDecoded->textures[1]->pixels->hash &&
                  conflictingDecoded->textures[1]->pixels->hash.rfind("proto-portable-image-v1:", 0) == 0,
              "Pixel key includes canonical pixels rather than declared hash");
        auto aliasedPixels = fixture();
        auto renamedPixels = std::make_shared<TexturePixels>(*aliasedPixels.textures[1]->pixels);
        renamedPixels->hash = "builtin:white";
        auto renamedTexture = std::make_shared<TextureAsset>(*aliasedPixels.textures[1]);
        renamedTexture->pixels = renamedPixels;
        aliasedPixels.textures[1] = renamedTexture;
        const auto aliasedBlob = encodePortableModel(aliasedPixels);
        const std::string builtinKey = "builtin:white";
        check(std::search(aliasedBlob.begin(), aliasedBlob.end(), builtinKey.begin(), builtinKey.end()) == aliasedBlob.end(),
              "Serialized pixel key does not trust builtin hash");
        const auto aliasedDecoded = decodePortableModel(aliasedBlob);
        check(aliasedDecoded->textures[0]->pixels == aliasedDecoded->textures[1]->pixels,
              "Identical canonical pixels retain sharing despite declared hash differences");

        std::cout << "PASS m6 portable model roundtrip and validation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL m6 portable model: " << error.what() << '\n';
        return 1;
    }
}
