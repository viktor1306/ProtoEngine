#include "assets/AssetWorkspace.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace proto {
std::vector<ImageMip> makeMips(uint32_t width, uint32_t height, std::vector<uint8_t> rgba, bool srgb, bool normal,
                               float cutoff) {
    if (!width || !height || width > 8192 || height > 8192 || rgba.size() != size_t(width) * height * 4)
        throw std::runtime_error("Invalid mip source dimensions");
    std::vector<ImageMip> result;
    result.push_back({width, height, std::move(rgba)});
    auto coverage = [&](const std::vector<uint8_t>& pixels, float multiplier) {
        size_t count{};
        for (size_t i = 3; i < pixels.size(); i += 4)
            if (float(std::clamp(std::lround(float(pixels[i]) * multiplier), 0l, 255l)) / 255 >= cutoff)
                ++count;
        return double(count) / double(pixels.size() / 4);
    };
    const double target = cutoff > 0 ? coverage(result[0].rgba, 1) : 0;
    while (width > 1 || height > 1) {
        const auto& previous = result.back();
        const auto w = std::max(1u, width / 2), h = std::max(1u, height / 2);
        ImageMip mip{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                glm::vec4 sum(0);
                const float x0 = float(x) * float(width) / float(w), x1 = float(x + 1) * float(width) / float(w);
                const float y0 = float(y) * float(height) / float(h), y1 = float(y + 1) * float(height) / float(h);
                for (uint32_t sy = static_cast<uint32_t>(y0); sy < static_cast<uint32_t>(std::ceil(y1)); ++sy)
                    for (uint32_t sx = static_cast<uint32_t>(x0); sx < static_cast<uint32_t>(std::ceil(x1)); ++sx) {
                        const float weight = (std::min(float(sx + 1), x1) - std::max(float(sx), x0)) *
                                             (std::min(float(sy + 1), y1) - std::max(float(sy), y0));
                        const size_t i = (size_t(std::min(height - 1, sy)) * width + std::min(width - 1, sx)) * 4;
                        for (int c = 0; c < 4; ++c) {
                            float v = float(previous.rgba[i + size_t(c)]) / 255;
                            if (srgb && c < 3)
                                v = v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
                            sum[c] += v * weight;
                        }
                    }
                sum /= (x1 - x0) * (y1 - y0);
                if (normal) {
                    glm::vec3 n = glm::vec3(sum) * 2.0f - 1.0f;
                    n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 0, 1);
                    sum.x = n.x * .5f + .5f;
                    sum.y = n.y * .5f + .5f;
                    sum.z = n.z * .5f + .5f;
                }
                for (int c = 0; c < 4; ++c) {
                    float v = sum[c];
                    if (srgb && c < 3)
                        v = v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
                    mip.rgba[(size_t(y) * w + x) * 4 + size_t(c)] =
                        static_cast<uint8_t>(std::clamp(std::lround(v * 255), 0l, 255l));
                }
            }
        if (cutoff > 0 && cutoff <= 1) {
            float lo = 0, hi = 8;
            for (int i = 0; i < 12; ++i) {
                float mid = (lo + hi) * .5f;
                if (coverage(mip.rgba, mid) < target)
                    lo = mid;
                else
                    hi = mid;
            }
            for (size_t i = 3; i < mip.rgba.size(); i += 4)
                mip.rgba[i] = static_cast<uint8_t>(std::clamp(std::lround(float(mip.rgba[i]) * hi), 0l, 255l));
        }
        result.push_back(std::move(mip));
        width = w;
        height = h;
    }
    return result;
}
namespace {
struct Writer {
    std::vector<uint8_t> data;
    template <class T> void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        data.insert(data.end(), p, p + sizeof(T));
    }
    void string(const std::string& s) {
        pod(uint64_t(s.size()));
        data.insert(data.end(), s.begin(), s.end());
    }
    template <class T> void array(const std::vector<T>& v) {
        pod(uint64_t(v.size()));
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        if (!v.empty())
            data.insert(data.end(), p, p + v.size() * sizeof(T));
    }
};
struct Reader {
    std::vector<uint8_t> data;
    size_t at{};
    template <class T> T pod() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (sizeof(T) > data.size() - at)
            throw std::runtime_error("Truncated cache");
        T v;
        std::memcpy(&v, data.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    uint64_t count(uint64_t maximum) {
        auto n = pod<uint64_t>();
        if (n > maximum)
            throw std::runtime_error("Cache count exceeds limit");
        return n;
    }
    std::string string() {
        auto n = count(1024 * 1024);
        if (n > data.size() - at)
            throw std::runtime_error("Truncated cache string");
        std::string s(reinterpret_cast<const char*>(data.data() + at), size_t(n));
        at += size_t(n);
        return s;
    }
    template <class T> std::vector<T> array() {
        auto n = count(512 * 1024 * 1024 / sizeof(T));
        if (n * sizeof(T) > data.size() - at)
            throw std::runtime_error("Truncated cache array");
        std::vector<T> v{std::vector<T>(size_t(n))};
        if (n)
            std::memcpy(v.data(), data.data() + at, size_t(n) * sizeof(T));
        at += size_t(n) * sizeof(T);
        return v;
    }
};
} // namespace
void saveCooked(const std::filesystem::path& file, const ModelBundle& m) {
    Writer w;
    w.string("ProtoCook4-win64");
    w.pod(m.id);
    w.string(m.name);
    w.string(m.source);
    w.string(m.revision);
    w.pod(uint64_t(m.nodes.size()));
    for (auto& n : m.nodes) {
        w.string(n.name);
        w.pod(n.parent);
        w.pod(n.local);
        w.pod(n.mesh);
    }
    w.pod(uint64_t(m.meshes.size()));
    for (auto& p : m.meshes) {
        w.pod(p->id);
        w.string(p->name);
        w.string(p->revision);
        w.pod(p->bounds);
        w.array(p->vertices);
        w.array(p->indices);
        w.array(p->parts);
        w.array(p->triangles);
        w.array(p->bvh);
    }
    std::unordered_map<std::string, uint64_t> unique;
    std::vector<std::shared_ptr<const TexturePixels>> pixels;
    for (auto& t : m.textures)
        if (!unique.contains(t->pixels->hash)) {
            unique[t->pixels->hash] = pixels.size();
            pixels.push_back(t->pixels);
        }
    w.pod(uint64_t(pixels.size()));
    for (auto& p : pixels) {
        w.string(p->hash);
        w.pod(p->srgb);
        w.pod(uint64_t(p->mips.size()));
        for (auto& level : p->mips) {
            w.pod(level.width);
            w.pod(level.height);
            w.array(level.rgba);
        }
    }
    w.pod(uint64_t(m.textures.size()));
    for (auto& t : m.textures) {
        w.pod(t->id);
        w.pod(unique.at(t->pixels->hash));
        w.pod(t->minFilter);
        w.pod(t->magFilter);
        w.pod(t->wrapS);
        w.pod(t->wrapT);
    }
    w.pod(uint64_t(m.materials.size()));
    for (auto& a : m.materials) {
        w.pod(a->id);
        w.pod(a->owner);
        w.string(a->name);
        w.pod(a->values);
        w.pod(a->textures);
    }
    w.pod(uint64_t(m.warnings.size()));
    for (auto& s : m.warnings)
        w.string(s);
    if (w.data.size() > 512 * 1024 * 1024)
        throw std::runtime_error("Cooked resource budget exceeded");
    auto staged = file;
    staged += L".tmp";
    {
        std::ofstream out(nativeFilePath(staged), std::ios::binary);
        out.write(reinterpret_cast<const char*>(w.data.data()), static_cast<std::streamsize>(w.data.size()));
        out.flush();
        if (!out)
            throw std::runtime_error("Cache write failed");
    }
    if (!MoveFileExW(nativeFilePath(staged).c_str(), nativeFilePath(file).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cache publication failed");
}
std::shared_ptr<ModelBundle> readCooked(const std::filesystem::path& file) {
    Reader r{assetBytes(file, 512 * 1024 * 1024)};
    if (r.string() != "ProtoCook4-win64")
        throw std::runtime_error("Cache version mismatch");
    auto m = std::make_shared<ModelBundle>();
    m->id = r.pod<AssetId>();
    m->name = r.string();
    m->source = r.string();
    m->revision = r.string();
    for (auto n = r.count(100000); n--;) {
        ModelNode node;
        node.name = r.string();
        node.parent = r.pod<int>();
        node.local = r.pod<glm::mat4>();
        node.mesh = r.pod<AssetId>();
        if (node.parent >= static_cast<int>(m->nodes.size()))
            throw std::runtime_error("Invalid cached hierarchy");
        m->nodes.push_back(node);
    }
    for (auto n = r.count(10000); n--;) {
        auto mesh = std::make_shared<MeshAsset>();
        mesh->id = r.pod<AssetId>();
        mesh->name = r.string();
        mesh->revision = r.string();
        mesh->bounds = r.pod<Bounds>();
        mesh->vertices = r.array<AssetVertex>();
        mesh->indices = r.array<uint32_t>();
        mesh->parts = r.array<MeshPart>();
        mesh->triangles = r.array<MeshTriangle>();
        mesh->bvh = r.array<MeshBvhNode>();
        for (auto i : mesh->indices)
            if (i >= mesh->vertices.size())
                throw std::runtime_error("Invalid cached index");
        m->meshes.push_back(mesh);
    }
    std::vector<std::shared_ptr<TexturePixels>> pixels;
    for (auto n = r.count(20000); n--;) {
        auto p = std::make_shared<TexturePixels>();
        p->hash = r.string();
        p->srgb = r.pod<bool>();
        for (auto k = r.count(14); k--;) {
            ImageMip mip;
            mip.width = r.pod<uint32_t>();
            mip.height = r.pod<uint32_t>();
            mip.rgba = r.array<uint8_t>();
            if (!mip.width || !mip.height || mip.width > 8192 || mip.height > 8192 ||
                mip.rgba.size() != size_t(mip.width) * mip.height * 4)
                throw std::runtime_error("Invalid cached texture");
            p->mips.push_back(std::move(mip));
        }
        if (p->mips.empty())
            throw std::runtime_error("Empty cached texture");
        pixels.push_back(p);
    }
    for (auto n = r.count(20000); n--;) {
        auto t = std::make_shared<TextureAsset>();
        t->id = r.pod<AssetId>();
        auto i = r.pod<uint64_t>();
        if (i >= pixels.size())
            throw std::runtime_error("Invalid cached texture link");
        t->pixels = pixels[size_t(i)];
        t->minFilter = r.pod<int>();
        t->magFilter = r.pod<int>();
        t->wrapS = r.pod<int>();
        t->wrapT = r.pod<int>();
        m->textures.push_back(t);
    }
    for (auto n = r.count(4097); n--;) {
        auto a = std::make_shared<MaterialAsset>();
        a->id = r.pod<AssetId>();
        a->owner = r.pod<AssetId>();
        a->name = r.string();
        a->values = r.pod<MaterialValues>();
        a->textures = r.pod<std::array<TextureSlot, 5>>();
        validateMaterial(a->values);
        m->materials.push_back(a);
    }
    for (auto n = r.count(1024); n--;)
        m->warnings.push_back(r.string());
    if (r.at != r.data.size())
        throw std::runtime_error("Trailing cache bytes");
    m->fromCache = true;
    return m;
}
} // namespace proto
