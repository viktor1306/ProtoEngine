#include "AssetFixtures.hpp"
#include "assets/AssetTypes.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <numbers>

namespace proto::fixtures {
namespace {
void put(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw std::runtime_error("Fixture write failed");
}
void text(const std::filesystem::path& path, const std::string& value) {
    put(path, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}
void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void imageWrite(void* context, void* data, int size) {
    auto& out = *static_cast<std::vector<uint8_t>*>(context);
    const auto* begin = static_cast<const uint8_t*>(data);
    out.insert(out.end(), begin, begin + size);
}
std::string base64(const std::vector<uint8_t>& bytes) {
    constexpr const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        result += table[n >> 18];
        result += table[(n >> 12) & 63];
        result += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? table[n & 63] : '=';
    }
    return result;
}
void createShowcase(const std::filesystem::path& directory) {
    constexpr unsigned rings = 32, segments = 64;
    std::vector<AssetVertex> sphere;
    std::vector<uint32_t> indices;
    for (unsigned y = 0; y <= rings; ++y)
        for (unsigned x = 0; x <= segments; ++x) {
            const float theta = float(y) / rings * std::numbers::pi_v<float>,
                        phi = float(x) / segments * 2 * std::numbers::pi_v<float>;
            AssetVertex v;
            v.normal = {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            v.position = v.normal * .9f;
            v.uv0 = {float(x) / segments, float(y) / rings};
            sphere.push_back(v);
        }
    for (unsigned y = 0; y < rings; ++y)
        for (unsigned x = 0; x < segments; ++x) {
            const auto a = y * (segments + 1) + x, b = a + 1, c = a + segments + 1, d = c + 1;
            if (y)
                indices.insert(indices.end(), {a, b, c});
            if (y + 1 < rings)
                indices.insert(indices.end(), {b, d, c});
        }
    std::vector<uint8_t> binary;
    const auto append = [&](const auto& values) {
        const size_t offset = binary.size();
        const auto* begin = reinterpret_cast<const uint8_t*>(values.data());
        binary.insert(binary.end(), begin, begin + values.size() * sizeof(values[0]));
        return offset;
    };
    append(sphere);
    const auto indexOffset = append(indices);
    std::vector<uint8_t> pixels(128 * 128 * 4);
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 128; ++x) {
            const float wave = .18f * std::sin(float(x) * .35f) * std::cos(float(y) * .35f);
            const glm::vec3 normal = glm::normalize(glm::vec3(wave, -wave, 1));
            const size_t at = size_t(y * 128 + x) * 4;
            for (int c = 0; c < 3; ++c)
                pixels[at + size_t(c)] = static_cast<uint8_t>(std::lround((normal[c] * .5f + .5f) * 255));
            pixels[at + 3] = 255;
        }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(imageWrite, &png, 128, 128, 4, pixels.data(), 128 * 4);
    const auto imageOffset = append(png);
    const auto binaryLength = binary.size();
    while (binary.size() % 4)
        binary.push_back(0);
    std::ostringstream out;
    out << R"({"asset":{"version":"2.0","generator":"Proto Engine original material spheres"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Сфери матеріалів","children":[1,2,3,4]},{"name":"Синя кераміка","mesh":0,"translation":[-2.1,1,0]},{"name":"Золото","mesh":0,"translation":[0,1,0]},{"name":"Шорстка мідь","mesh":0,"translation":[2.1,1,0]},{"name":"Дзеркальна кераміка","mesh":0,"translation":[0,1,-2.4],"scale":[-1,1,1]}],"buffers":[{"byteLength":)"
        << binaryLength << "}],";
    out << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" << sphere.size() * sizeof(AssetVertex)
        << R"(,"byteStride":)" << sizeof(AssetVertex) << "},";
    out << R"({"buffer":0,"byteOffset":)" << indexOffset << R"(,"byteLength":)" << indices.size() * 4 << "},";
    out << R"({"buffer":0,"byteOffset":)" << imageOffset << R"(,"byteLength":)" << png.size() << "}],";
    out << R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" << sphere.size()
        << R"(,"type":"VEC3","min":[-0.9,-0.9,-0.9],"max":[0.9,0.9,0.9]},)";
    out << R"({"bufferView":0,"byteOffset":)" << offsetof(AssetVertex, normal) << R"(,"componentType":5126,"count":)"
        << sphere.size() << R"(,"type":"VEC3"},)";
    out << R"({"bufferView":0,"byteOffset":)" << offsetof(AssetVertex, uv0) << R"(,"componentType":5126,"count":)"
        << sphere.size() << R"(,"type":"VEC2"},)";
    out << R"({"bufferView":1,"componentType":5125,"count":)" << indices.size()
        << R"(,"type":"SCALAR"}],"images":[{"bufferView":2,"mimeType":"image/png"}],"textures":[{"source":0}],)";
    out << R"("materials":[{"name":"Синя кераміка","pbrMetallicRoughness":{"baseColorFactor":[0.025,0.22,0.65,1],"metallicFactor":0,"roughnessFactor":0.26},"normalTexture":{"index":0,"scale":0.65}},{"name":"Золото","pbrMetallicRoughness":{"baseColorFactor":[1,0.66,0.19,1],"metallicFactor":1,"roughnessFactor":0.28}},{"name":"Шорстка мідь","pbrMetallicRoughness":{"baseColorFactor":[0.82,0.3,0.1,1],"metallicFactor":0.85,"roughnessFactor":0.6}}],"meshes":[{"name":"Сфера · спільна геометрія","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}]})";
    auto json = out.str();
    while (json.size() % 4)
        json += ' ';
    std::vector<uint8_t> glb;
    append32(glb, 0x46546c67);
    append32(glb, 2);
    append32(glb, static_cast<uint32_t>(28 + json.size() + binary.size()));
    append32(glb, static_cast<uint32_t>(json.size()));
    append32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    append32(glb, static_cast<uint32_t>(binary.size()));
    append32(glb, 0x004e4942);
    glb.insert(glb.end(), binary.begin(), binary.end());
    put(directory / "MaterialSpheres.glb", glb);
}
} // namespace
void create(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory / L"текстури");
    std::vector<uint8_t> mask(16 * 16 * 4), normal(1024 * 1024 * 4), checker(16 * 16 * 4), packed(16 * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const size_t at = size_t(y * 16 + x) * 4;
            mask[at] = 80;
            mask[at + 1] = 200;
            mask[at + 2] = 120;
            mask[at + 3] = x < 8 ? 0 : 255;
            checker[at] = y >= 8 || x < 8 ? 255 : 0;
            checker[at + 1] = x >= 8 ? 255 : 0;
            checker[at + 2] = y >= 8 ? 255 : 0;
            checker[at + 3] = 255;
            packed[at] = 40;
            packed[at + 1] = 153;
            packed[at + 2] = 200;
            packed[at + 3] = 255;
        }
    for (size_t at = 0; at < normal.size(); at += 4) {
        normal[at] = 204;
        normal[at + 1] = 128;
        normal[at + 2] = 230;
        normal[at + 3] = 255;
    }
    std::vector<uint8_t> maskPng, normalPng, jpeg, packedPng;
    stbi_write_png_to_func(imageWrite, &maskPng, 16, 16, 4, mask.data(), 16 * 4);
    stbi_write_png_to_func(imageWrite, &normalPng, 1024, 1024, 4, normal.data(), 1024 * 4);
    stbi_write_jpg_to_func(imageWrite, &jpeg, 16, 16, 4, checker.data(), 100);
    stbi_write_png_to_func(imageWrite, &packedPng, 16, 16, 4, packed.data(), 16 * 4);
    put(directory / L"текстури/маска 100%.png", maskPng);
    put(directory / "normal.png", normalPng);
    put(directory / "checker.jpg", jpeg);

    std::array<AssetVertex, 4> vertices{};
    vertices[0].position = {-.85f, -.85f, 0};
    vertices[0].uv0 = {0, 1};
    vertices[1].position = {.85f, -.85f, 0};
    vertices[1].uv0 = {1, 1};
    vertices[2].position = {.85f, .85f, 0};
    vertices[2].uv0 = {1, 0};
    vertices[3].position = {-.85f, .85f, 0};
    vertices[3].uv0 = {0, 0};
    for (auto& vertex : vertices)
        vertex.normal = {0, 0, 1};
    const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
    std::vector<uint8_t> binary;
    const auto append = [&](const auto& value) {
        const auto* data = reinterpret_cast<const uint8_t*>(value.data());
        binary.insert(binary.end(), data, data + sizeof(value));
    };
    append(vertices);
    const size_t indexOffset = binary.size();
    append(indices);
    const std::array<uint16_t, 8> uv1{65535, 65535, 0, 65535, 0, 0, 65535, 0};
    const size_t uvOffset = binary.size();
    append(uv1);
    const std::array<uint8_t, 16> colors{255, 255, 255, 255, 255, 255, 255, 255,
                                         255, 255, 255, 255, 255, 255, 255, 255};
    const size_t colorOffset = binary.size();
    append(colors);
    auto halves = vertices;
    for (auto& v : halves)
        v.position.x = v.position.x * .45f - .45f;
    const size_t leftOffset = binary.size();
    append(halves);
    halves = vertices;
    for (auto& v : halves)
        v.position.x = v.position.x * .45f + .45f;
    const size_t rightOffset = binary.size();
    append(halves);

    const auto json = [&](bool embedded, size_t imageOffset, size_t totalSize) {
        std::ostringstream out;
        out << R"({"asset":{"version":"2.0","generator":"Proto Engine deterministic M2 fixtures"},"extensionsUsed":["KHR_texture_transform","KHR_materials_unlit"],"scene":0,"scenes":[{"nodes":[0]}],"buffers":[{"byteLength":)"
            << totalSize;
        if (!embedded)
            out << R"(,"uri":"mesh%20data.bin")";
        out << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" << sizeof(vertices)
            << R"(,"byteStride":)" << sizeof(AssetVertex) << "},";
        out << R"({"buffer":0,"byteOffset":)" << indexOffset << R"(,"byteLength":24},)";
        out << R"({"buffer":0,"byteOffset":)" << uvOffset << R"(,"byteLength":16},)";
        out << R"({"buffer":0,"byteOffset":)" << colorOffset << R"(,"byteLength":16},)";
        out << R"({"buffer":0,"byteOffset":)" << leftOffset << R"(,"byteLength":)" << sizeof(vertices)
            << R"(,"byteStride":)" << sizeof(AssetVertex) << "},";
        out << R"({"buffer":0,"byteOffset":)" << rightOffset << R"(,"byteLength":)" << sizeof(vertices)
            << R"(,"byteStride":)" << sizeof(AssetVertex) << "}";
        if (embedded)
            out << R"(,{"buffer":0,"byteOffset":)" << imageOffset << R"(,"byteLength":)" << maskPng.size() << "}";
        out << R"(],"accessors":[{"bufferView":0,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC3","min":[-0.85,-0.85,0],"max":[0.85,0.85,0]},)";
        out << R"({"bufferView":0,"byteOffset":)" << offsetof(AssetVertex, uv0)
            << R"(,"componentType":5126,"count":4,"type":"VEC2"},)";
        out << R"({"bufferView":2,"componentType":5123,"normalized":true,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5121,"normalized":true,"count":4,"type":"VEC4"},{"bufferView":1,"componentType":5125,"count":6,"type":"SCALAR"},)";
        out << R"({"bufferView":4,"componentType":5126,"count":4,"type":"VEC3","min":[-0.8325,-0.85,0],"max":[-0.0675,0.85,0]},{"bufferView":5,"componentType":5126,"count":4,"type":"VEC3","min":[0.0675,-0.85,0],"max":[0.8325,0.85,0]}],"images":[)";
        out << (embedded ? R"({"bufferView":6,"mimeType":"image/png"})" : R"({"uri":"текстури/маска%20100%25.png"})");
        out << R"(,{"uri":"normal.png"},{"uri":"checker.jpg"},{"uri":"data:image/png;base64,)" << base64(packedPng)
            << R"("}],"samplers":[{"magFilter":9728,"minFilter":9984,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0},{"source":1,"sampler":0},{"source":2,"sampler":0},{"source":3,"sampler":0}],"materials":[)";
        out << R"({"name":"Normal + mirror","doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[0.45,0.45,0.45,1],"metallicFactor":0,"roughnessFactor":0.8},"normalTexture":{"index":1}},)";
        out << R"({"name":"Flat reference","pbrMetallicRoughness":{"baseColorFactor":[0.45,0.45,0.45,1],"metallicFactor":0,"roughnessFactor":0.8}},)";
        out << R"({"name":"Cutout / MASK","alphaMode":"MASK","alphaCutoff":0.5,"doubleSided":true,"extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},)";
        out << R"({"name":"Gold / packed MR","pbrMetallicRoughness":{"baseColorFactor":[1,0.55,0.1,1],"metallicFactor":1,"roughnessFactor":0.4,"metallicRoughnessTexture":{"index":3}}},)";
        out << R"({"name":"JPEG / UV transform","extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{"baseColorTexture":{"index":2,"extensions":{"KHR_texture_transform":{"offset":[1,0],"scale":[0.5,0.5],"rotation":1.57079632679,"texCoord":1}}}}},)";
        out << R"({"name":"Opaque ignores alpha","extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],"meshes":[)";
        const auto primitive = [&](int position, int material) {
            out << R"({"attributes":{"POSITION":)" << position
                << R"(,"TEXCOORD_0":1,"TEXCOORD_1":2,"COLOR_0":3},"indices":4,"material":)" << material << "}";
        };
        for (int mesh = 0; mesh < 6; ++mesh) {
            if (mesh)
                out << ',';
            out << R"({"name":"Mesh )" << mesh << R"(","primitives":[)";
            if (mesh == 3) {
                primitive(5, 1);
                out << ',';
                primitive(6, 3);
            } else
                primitive(0, mesh);
            out << "]}";
        }
        out << R"(],"nodes":[{"name":"Material Lab","children":[1,2,3,4,5,6,7,8,9]},)";
        out << R"({"name":"Normal map","mesh":0,"translation":[-2.4,1.4,0]},{"name":"Mirrored normal","mesh":0,"translation":[0,1.4,0],"scale":[-1,1,1]},{"name":"Flat normal","mesh":1,"translation":[2.4,1.4,0]},)";
        out << R"({"name":"MASK","mesh":2,"translation":[-2.4,-1,0]},{"name":"Mirrored MASK","mesh":2,"translation":[0,-1,0],"scale":[-1,1,1]},{"name":"Texture transform","mesh":4,"translation":[2.4,-1,0]},)";
        out << R"({"name":"Back face","mesh":0,"translation":[-2.4,-3.4,0],"rotation":[0,1,0,0]},{"name":"Two material slots","mesh":3,"translation":[0,-3.4,0]},{"name":"OPAQUE","mesh":5,"translation":[2.4,-3.4,0]}]})";
        return out.str();
    };
    put(directory / "mesh data.bin", binary);
    text(directory / "probes.gltf", json(false, 0, binary.size()));
    auto glbBinary = binary;
    const auto imageOffset = glbBinary.size();
    glbBinary.insert(glbBinary.end(), maskPng.begin(), maskPng.end());
    const auto byteLength = glbBinary.size();
    while (glbBinary.size() % 4)
        glbBinary.push_back(0);
    auto glbJson = json(true, imageOffset, byteLength);
    while (glbJson.size() % 4)
        glbJson += ' ';
    std::vector<uint8_t> glb;
    append32(glb, 0x46546c67);
    append32(glb, 2);
    append32(glb, static_cast<uint32_t>(12 + 8 + glbJson.size() + 8 + glbBinary.size()));
    append32(glb, static_cast<uint32_t>(glbJson.size()));
    append32(glb, 0x4e4f534a);
    glb.insert(glb.end(), glbJson.begin(), glbJson.end());
    append32(glb, static_cast<uint32_t>(glbBinary.size()));
    append32(glb, 0x004e4942);
    glb.insert(glb.end(), glbBinary.begin(), glbBinary.end());
    put(directory / "embedded.glb", glb);
    createShowcase(directory);
    std::ostringstream manifest;
    manifest << "# M2 fixture provenance\n\nSource: tests/AssetFixtures.cpp in Proto Engine. Original, deterministic "
                "geometry and textures.\nLicense: CC0-1.0 (dedicated to the public domain). No external assets.\n\n";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().filename() != "MANIFEST.md")
            manifest << "- `" << utf8(entry.path().lexically_relative(directory).generic_wstring()) << "`: `"
                     << sha256(assetBytes(entry.path())) << "`\n";
    text(directory / "MANIFEST.md", manifest.str());
}
} // namespace proto::fixtures
