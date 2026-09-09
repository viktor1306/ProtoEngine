#include "assets/AssetWorkspace.hpp"
#include "assets/AssetIO.hpp"
#include "scene/SceneIO.hpp"
#include "core/Diagnostics.hpp"
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <mikktspace.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>

namespace proto {
namespace {
std::wstring extension(const std::filesystem::path& path) {
    auto value = path.extension().wstring();
    for (auto& character : value)
        if (character >= L'A' && character <= L'Z')
            character += L'a' - L'A';
    return value;
}
struct Meta {
    struct Dependency {
        std::string uri, hash, assetId, pointer;
    };
    AssetId id{AssetId::create()};
    std::string signature, content, cacheHash, previous, overrides{"{}"};
    std::map<std::string, AssetId> ids;
    std::vector<Dependency> dependencies;
    bool createdSubAsset{};
    AssetId sub(const std::string& locator) {
        auto [it, added] = ids.try_emplace(locator);
        if (added) {
            it->second = AssetId::create();
            createdSubAsset = true;
        }
        return it->second;
    }
};
Meta readMeta(const std::filesystem::path& path) {
    Meta m;
    if (!std::filesystem::exists(path))
        return m;
    m.previous = readDocument(path);
    JsonDoc doc(m.previous);
    auto* r = doc.root();
    if (str(get(r, "format")) != "proto.assetmeta" || num(get(r, "formatVersion")) != 1 ||
        str(get(r, "kind")) != "ModelSource")
        throw std::runtime_error("Unsupported asset metadata");
    auto* importer = get(r, "importer");
    if (str(get(importer, "name")) != "gltf" || num(get(importer, "version")) != 2 ||
        !yyjson_is_true(get(get(r, "settings"), "generateTangents")))
        throw std::runtime_error("Unsupported importer version/settings");
    if (!yyjson_is_arr(get(r, "subAssets")) || !yyjson_is_arr(get(r, "dependencies")))
        throw std::runtime_error("Invalid metadata lists");
    m.id = AssetId::parse(str(get(r, "assetId")));
    m.signature = str(get(r, "signature"));
    m.content = str(get(r, "contentHash"));
    m.cacheHash = str(get(r, "cacheHash"));
    size_t i, n;
    Json* v;
    yyjson_arr_foreach(get(r, "subAssets"), i, n, v) {
        const auto key = str(get(v, "locator"));
        if (!m.ids.emplace(key, AssetId::parse(str(get(v, "id")))).second)
            throw std::runtime_error("Duplicate subresource locator");
    }
    yyjson_arr_foreach(get(r, "dependencies"), i, n, v) {
        if (!yyjson_is_obj(v))
            throw std::runtime_error("Invalid asset dependency");
        Meta::Dependency dependency{str(get(v, "uri")), str(get(v, "sha256")), {}, {}};
        if (auto* id = yyjson_obj_get(v, "assetId")) {
            dependency.assetId = str(id);
            AssetId::parse(dependency.assetId);
        }
        if (auto* pointer = yyjson_obj_get(v, "pointer"))
            dependency.pointer = str(pointer);
        else if (auto* pointer = yyjson_obj_get(v, "jsonPointer"))
            dependency.pointer = str(pointer);
        m.dependencies.push_back(std::move(dependency));
    }
    if (auto* v = yyjson_obj_get(r, "materialOverrides")) {
        if (!yyjson_is_obj(v))
            throw std::runtime_error("Invalid material overrides");
        char* raw = yyjson_val_write(v, 0, nullptr);
        if (!raw)
            throw std::bad_alloc();
        m.overrides = raw;
        free(raw);
    }
    return m;
}
std::string emitMeta(const Meta& m, const std::vector<Meta::Dependency>& deps) {
    std::ostringstream o;
    o << "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"kind\":\"ModelSource\",\"assetId\":"
      << jsonString(m.id.string())
      << ",\"importer\":{\"name\":\"gltf\",\"version\":2},\"settings\":{\"generateTangents\":true},\"signature\":"
      << jsonString(m.signature) << ",\"contentHash\":" << jsonString(m.content)
      << ",\"cacheHash\":" << jsonString(m.cacheHash) << ",\"dependencies\":[";
    bool comma = false;
    for (const auto& dependency : deps) {
        if (comma)
            o << ',';
        comma = true;
        o << "{\"uri\":" << jsonString(dependency.uri) << ",\"sha256\":" << jsonString(dependency.hash);
        if (!dependency.assetId.empty())
            o << ",\"assetId\":" << jsonString(dependency.assetId);
        if (!dependency.pointer.empty())
            o << ",\"pointer\":" << jsonString(dependency.pointer);
        o << '}';
    }
    o << "],\"subAssets\":[";
    comma = false;
    for (auto& [locator, id] : m.ids) {
        if (comma)
            o << ',';
        comma = true;
        const char* kind = locator.starts_with("/meshes/") ? "Mesh"
                           : locator.starts_with("/materials/") && locator.find("texture") == std::string::npos
                               ? "Material"
                               : "Texture";
        o << "{\"id\":" << jsonString(id.string()) << ",\"kind\":" << jsonString(kind)
          << ",\"locator\":" << jsonString(locator) << '}';
    }
    o << "],\"materialOverrides\":" << m.overrides << "}\n";
    return o.str();
}
void overrides(ModelBundle& bundle, const Meta& meta) {
    JsonDoc doc(meta.overrides);
    for (auto& item : bundle.materials) {
        auto* v = yyjson_obj_get(doc.root(), item->id.string().c_str());
        if (!v)
            continue;
        auto m = std::make_shared<MaterialAsset>(*item);
        auto* base = get(v, "baseColor");
        if (yyjson_arr_size(base) != 4)
            throw std::runtime_error("Invalid material override");
        for (int i = 0; i < 4; ++i)
            m->values.baseColor[i] = static_cast<float>(num(yyjson_arr_get(base, static_cast<size_t>(i))));
        m->values.metallic = static_cast<float>(num(get(v, "metallic")));
        m->values.roughness = static_cast<float>(num(get(v, "roughness")));
        m->values.normalScale = static_cast<float>(num(get(v, "normalScale")));
        validateMaterial(m->values);
        item = m;
    }
}
std::vector<float> floats(const cgltf_accessor* a, size_t count, int components) {
    if (!a)
        return {};
    if (a->count != count || cgltf_num_components(a->type) != static_cast<size_t>(components))
        throw std::runtime_error("Accessor shape mismatch");
    std::vector<float> data(count * static_cast<size_t>(components));
    if (cgltf_accessor_unpack_floats(a, data.data(), data.size()) != data.size())
        throw std::runtime_error("Accessor decode failed");
    for (float f : data)
        if (!std::isfinite(f))
            throw std::runtime_error("Non-finite mesh attribute");
    return data;
}
void tangents(std::vector<AssetVertex>& vertices, int uv) {
    struct Work {
        std::vector<AssetVertex>* v;
        int uv;
    };
    Work work{&vertices, uv};
    SMikkTSpaceInterface api{};
    auto vertex = [](const SMikkTSpaceContext* c, int f, int v) -> AssetVertex& {
        return (*static_cast<Work*>(c->m_pUserData)->v)[static_cast<size_t>(f) * 3 + static_cast<size_t>(v)];
    };
    (void)vertex;
    api.m_getNumFaces = [](const SMikkTSpaceContext* c) {
        return static_cast<int>(static_cast<Work*>(c->m_pUserData)->v->size() / 3);
    };
    api.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    api.m_getPosition = [](const SMikkTSpaceContext* c, float out[], int f, int v) {
        const auto& p = (*static_cast<Work*>(c->m_pUserData)->v)[size_t(f) * 3 + size_t(v)].position;
        std::memcpy(out, &p, 12);
    };
    api.m_getNormal = [](const SMikkTSpaceContext* c, float out[], int f, int v) {
        const auto& p = (*static_cast<Work*>(c->m_pUserData)->v)[size_t(f) * 3 + size_t(v)].normal;
        std::memcpy(out, &p, 12);
    };
    api.m_getTexCoord = [](const SMikkTSpaceContext* c, float out[], int f, int v) {
        auto* w = static_cast<Work*>(c->m_pUserData);
        const auto& p = (*w->v)[size_t(f) * 3 + size_t(v)];
        const auto& t = w->uv ? p.uv1 : p.uv0;
        std::memcpy(out, &t, 8);
    };
    api.m_setTSpaceBasic = [](const SMikkTSpaceContext* c, const float t[], float sign, int f, int v) {
        (*static_cast<Work*>(c->m_pUserData)->v)[size_t(f) * 3 + size_t(v)].tangent = {t[0], t[1], t[2], sign};
    };
    SMikkTSpaceContext context{&api, &work};
    if (!genTangSpaceDefault(&context))
        throw std::runtime_error("MikkTSpace tangent generation failed");
}
} // namespace
AssetWorkspace::AssetWorkspace(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()) {
    if (root_.has_relative_path() && root_.filename().empty())
        root_ = root_.parent_path();
}
namespace {
std::vector<std::pair<AssetId, std::string>> scanRegistry(const std::filesystem::path& root) {
    std::vector<std::pair<AssetId, std::string>> found;
    std::set<AssetId> all;
    const auto assets = root / "Assets";
    if (!std::filesystem::exists(assets))
        return found;
    for (const auto& e : std::filesystem::recursive_directory_iterator(assets)) {
        if (e.is_symlink())
            throw std::runtime_error("Asset folder must not contain links");
        if (!e.is_regular_file() || extension(e.path()) != L".meta")
            continue;
        auto source = e.path();
        source.replace_extension();
        if (extension(source) != L".gltf" && extension(source) != L".glb")
            continue;
        auto m = readMeta(e.path());
        if (!std::filesystem::exists(source) || !all.insert(m.id).second)
            throw std::runtime_error("Duplicate or missing model source");
        for (auto& [key, id] : m.ids)
            if (!all.insert(id).second)
                throw std::runtime_error("Duplicate subresource UUID");
        found.emplace_back(m.id, utf8(source.lexically_relative(root).generic_wstring()));
    }
    return found;
}
} // namespace
std::vector<std::pair<AssetId, std::string>> AssetWorkspace::rebuildRegistry() {
    const auto found = scanRegistry(root_);
    std::ostringstream out;
    out << "{\"format\":\"proto.registry\",\"formatVersion\":1,\"sources\":[";
    bool comma = false;
    for (auto& [id, path] : found) {
        if (comma)
            out << ',';
        comma = true;
        out << "{\"id\":" << jsonString(id.string()) << ",\"path\":" << jsonString(path) << '}';
    }
    out << "]}";
    std::filesystem::create_directories(root_ / ".proto");
    atomicWrite(root_ / ".proto/registry.json", out.str());
    return found;
}
std::shared_ptr<const ModelBundle> AssetWorkspace::load(AssetId id) {
    for (auto& [found, path] : rebuildRegistry())
        if (found == id)
            return cook(resourcePath(root_, path), false);
    throw std::runtime_error("Model SourceId not found in resource folder");
}
std::shared_ptr<const ModelBundle> AssetWorkspace::importFile(const std::filesystem::path& file) {
    return cook(std::filesystem::absolute(file), true);
}
std::shared_ptr<const ModelBundle> AssetWorkspace::cook(const std::filesystem::path& source, bool importing) {
    if (extension(source) != L".gltf" && extension(source) != L".glb")
        throw std::runtime_error("Оберіть glTF або GLB");
    auto raw = assetBytes(source);
    cgltf_options options{};
    cgltf_data* parsed{};
    if (cgltf_parse(&options, raw.data(), raw.size(), &parsed) != cgltf_result_success)
        throw std::runtime_error("Пошкоджений glTF/GLB");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    auto* d = data.get();
    for (size_t i = 0; i < d->extensions_required_count; ++i) {
        std::string ext = d->extensions_required[i];
        if (ext != "KHR_texture_transform" && ext != "KHR_materials_unlit" && ext != "KHR_materials_emissive_strength")
            throw std::runtime_error("Непідтримуване required extension: " + ext);
    }
    if (d->skins_count)
        throw std::runtime_error("Skeletal meshes are deferred");
    std::map<std::string, std::vector<uint8_t>> external;
    std::string content = sha256(raw) + "proto-cook-4";
    auto bytesFor = [&](const char* uri) {
        if (!uri)
            throw std::runtime_error("Missing resource URI");
        std::string key = uri;
        if (key.starts_with("data:"))
            return dataUri(key);
        auto it = external.find(key);
        if (it == external.end()) {
            const auto resolved = importing ? assetUri(source.parent_path(), key)
                                            : assetUriWithin(source.parent_path(), key, root_ / "Assets");
            it = external.emplace(key, assetBytes(resolved)).first;
        }
        return it->second;
    };
    std::vector<std::vector<uint8_t>> buffers(d->buffers_count), images(d->images_count);
    for (size_t i = 0; i < d->buffers_count; ++i) {
        auto& b = d->buffers[i];
        if (b.uri)
            buffers[i] = bytesFor(b.uri);
        else if (i == 0 && d->bin) {
            const auto* p = static_cast<const uint8_t*>(d->bin);
            buffers[i].assign(p, p + d->bin_size);
        } else
            throw std::runtime_error("Missing glTF buffer");
        if (buffers[i].size() < b.size)
            throw std::runtime_error("Truncated glTF buffer");
        b.data = buffers[i].data();
        b.data_free_method = cgltf_data_free_method_none;
    }
    if (cgltf_validate(d) != cgltf_result_success)
        throw std::runtime_error("glTF validation failed (accessors, bounds or references)");
    for (size_t i = 0; i < d->images_count; ++i) {
        const auto& im = d->images[i];
        if (im.uri)
            images[i] = bytesFor(im.uri);
        else if (im.buffer_view) {
            const auto* v = im.buffer_view;
            const auto* p = static_cast<const uint8_t*>(v->buffer->data) + v->offset;
            images[i].assign(p, p + v->size);
        } else
            throw std::runtime_error("Missing glTF image");
    }
    std::vector<Meta::Dependency> deps;
    for (auto& [uri, bytes] : external) {
        const auto hash = sha256(bytes);
        content += uri + hash;
        deps.push_back({uri, hash, {}, {}});
    }
    content = sha256(content);
    if (importing) {
        for (auto& [id, path] : rebuildRegistry()) {
            auto metaPath = resourcePath(root_, path);
            metaPath += L".meta";
            if (readMeta(metaPath).content == content) {
                try {
                    const auto current = cook(resourcePath(root_, path), false);
                    if (current->revision == content)
                        return current;
                } catch (const std::exception&) {
                    // A stale or damaged existing package is not the newly selected source.
                }
            }
        }
    }
    auto metaPath = source;
    metaPath += L".meta";
    Meta meta = importing ? Meta{} : readMeta(metaPath);
    if (!importing && meta.previous.empty())
        throw std::runtime_error("Resource metadata missing");
    for (auto& dependency : deps)
        for (const auto& previous : meta.dependencies)
            if (dependency.uri == previous.uri) {
                dependency.assetId = previous.assetId;
                dependency.pointer = previous.pointer;
                break;
            }
    std::ostringstream shape;
    for (size_t i = 0; i < d->meshes_count; ++i)
        shape << "m" << i << ':' << (d->meshes[i].name ? d->meshes[i].name : "") << ':' << d->meshes[i].primitives_count
              << ';';
    for (size_t i = 0; i < d->materials_count; ++i)
        shape << "a" << i << ':' << (d->materials[i].name ? d->materials[i].name : "") << ';';
    for (size_t i = 0; i < d->nodes_count; ++i)
        shape << "n" << i << ':' << (d->nodes[i].name ? d->nodes[i].name : "") << ':'
              << (d->nodes[i].parent ? d->nodes[i].parent - d->nodes : -1) << ':'
              << (d->nodes[i].mesh ? d->nodes[i].mesh - d->meshes : -1) << ';';
    const auto signature = sha256(shape.str());
    if (!meta.signature.empty() && meta.signature != signature)
        throw std::runtime_error(
            "Структура моделі змінилася: потрібен новий імпорт, щоб не перепризначити наявні UUID");
    const auto cacheKey = sha256(content + meta.id.string());
    const auto cacheFile = root_ / ".proto/cache" / (cacheKey + ".bin");
    auto cacheIntegrity = cacheFile;
    cacheIntegrity += L".hash";
    if (!importing && std::filesystem::exists(cacheFile))
        try {
            const auto cacheBytes = assetBytes(cacheFile, 512 * 1024 * 1024);
            const auto actualHash = sha256(cacheBytes);
            bool trusted = false;
            if (std::filesystem::exists(cacheIntegrity)) {
                auto recorded = readDocument(cacheIntegrity);
                while (!recorded.empty() && (recorded.back() == '\n' || recorded.back() == '\r' ||
                                             recorded.back() == ' ' || recorded.back() == '\t'))
                    recorded.pop_back();
                trusted = recorded == actualHash;
            } else if (meta.content == content && meta.cacheHash == actualHash) {
                // M2 cache fallback: old projects have no derived integrity sidecar.
                trusted = true;
            }
            if (trusted) {
                auto bundle = readCooked(cacheFile);
                if (bundle->id == meta.id && bundle->revision == content) {
                    bundle->source = utf8(source.lexically_relative(root_).generic_wstring());
                    bundle->fromCache = true;
                    overrides(*bundle, meta);
                    return bundle;
                }
            }
        } catch (const std::exception&) { /* Disposable cache: reconstruct from validated source. */
        }
    auto bundle = std::make_shared<ModelBundle>();
    bundle->id = meta.id;
    bundle->name = utf8(source.stem().wstring());
    bundle->revision = content;
    if (d->animations_count)
        bundle->warnings.push_back("Animation tracks skipped; imported static node pose");
    if (d->cameras_count)
        bundle->warnings.push_back("glTF cameras skipped; use a scene camera in the editor");
    for (size_t i = 0; i < d->extensions_used_count; ++i) {
        const std::string extension = d->extensions_used[i];
        if (extension != "KHR_texture_transform" && extension != "KHR_materials_unlit" &&
            extension != "KHR_materials_emissive_strength")
            bundle->warnings.push_back("Optional extension skipped; using core glTF fallback: " + extension);
    }
    if (d->meshes_count > 10000 || d->nodes_count > 100000 || d->materials_count > 4096)
        throw std::runtime_error("Model exceeds importer limits");
    std::vector<std::shared_ptr<ImageMip>> decoded(d->images_count);
    std::unordered_map<std::string, std::shared_ptr<const TexturePixels>> pixels;
    size_t decodedBytes{};
    auto texture = [&](const cgltf_texture_view& view, const std::string& locator, bool srgb, bool normal,
                       float cutoff) {
        TextureSlot slot;
        if (!view.texture)
            return slot;
        auto* t = view.texture;
        if (!t->image)
            throw std::runtime_error("Texture has no supported PNG/JPEG source");
        const size_t imageIndex = static_cast<size_t>(t->image - d->images);
        if (!decoded[imageIndex]) {
            const auto& bytes = images[imageIndex];
            int w, h, channels;
            if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) || w <= 0 ||
                h <= 0 || w > 8192 || h > 8192)
                throw std::runtime_error("Invalid or oversized PNG/JPEG");
            const size_t size = size_t(w) * size_t(h) * 4;
            if (decodedBytes + size > 512 * 1024 * 1024)
                throw std::runtime_error("Decoded texture budget exceeded");
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgba(
                stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4),
                stbi_image_free);
            if (!rgba)
                throw std::runtime_error("Image decode failed");
            auto im = std::make_shared<ImageMip>();
            im->width = static_cast<uint32_t>(w);
            im->height = static_cast<uint32_t>(h);
            im->rgba.assign(rgba.get(), rgba.get() + size);
            decoded[imageIndex] = im;
            decodedBytes += size;
            ++bundle->decodedImages;
        }
        const auto& im = *decoded[imageIndex];
        const auto key =
            sha256(images[imageIndex]) + std::to_string(srgb) + std::to_string(normal) + std::to_string(cutoff);
        auto found = pixels.find(key);
        if (found == pixels.end()) {
            auto p = std::make_shared<TexturePixels>();
            p->hash = sha256(key);
            p->srgb = srgb;
            p->mips = makeMips(im.width, im.height, im.rgba, srgb, normal, cutoff);
            found = pixels.emplace(key, p).first;
        }
        auto tex = std::make_shared<TextureAsset>();
        tex->id = meta.sub(locator);
        tex->pixels = found->second;
        if (t->sampler) {
            if (t->sampler->min_filter)
                tex->minFilter = t->sampler->min_filter;
            if (t->sampler->mag_filter)
                tex->magFilter = t->sampler->mag_filter;
            tex->wrapS = t->sampler->wrap_s;
            tex->wrapT = t->sampler->wrap_t;
        }
        bundle->textures.push_back(tex);
        slot.texture = tex->id;
        slot.texCoord = view.texcoord;
        if (view.has_transform) {
            slot.offset = {view.transform.offset[0], view.transform.offset[1]};
            slot.scale = {view.transform.scale[0], view.transform.scale[1]};
            slot.rotation = view.transform.rotation;
            if (view.transform.has_texcoord)
                slot.texCoord = view.transform.texcoord;
        }
        if (slot.texCoord < 0 || slot.texCoord > 1 || !std::isfinite(slot.rotation) || !std::isfinite(slot.offset.x) ||
            !std::isfinite(slot.offset.y) || !std::isfinite(slot.scale.x) || !std::isfinite(slot.scale.y))
            throw std::runtime_error("Unsupported UV set or invalid texture transform");
        return slot;
    };
    for (size_t i = 0; i <= d->materials_count; ++i) {
        auto mat = std::make_shared<MaterialAsset>();
        mat->id = meta.sub("/materials/" + std::to_string(i));
        mat->owner = meta.id;
        mat->name = "Default";
        if (i < d->materials_count) {
            const auto& m = d->materials[i];
            if (m.alpha_mode == cgltf_alpha_mode_blend)
                throw std::runtime_error("BLEND materials are deferred; use OPAQUE or MASK");
            mat->name = m.name ? m.name : "Material " + std::to_string(i);
            auto& v = mat->values;
            std::memcpy(&v.baseColor, m.pbr_metallic_roughness.base_color_factor, 16);
            std::memcpy(&v.emissive, m.emissive_factor, 12);
            if (m.has_emissive_strength)
                v.emissive *= m.emissive_strength.emissive_strength;
            v.metallic = m.pbr_metallic_roughness.metallic_factor;
            v.roughness = m.pbr_metallic_roughness.roughness_factor;
            v.normalScale = m.normal_texture.scale;
            v.occlusion = m.occlusion_texture.scale;
            v.mask = m.alpha_mode == cgltf_alpha_mode_mask;
            v.alphaCutoff = m.alpha_cutoff;
            v.doubleSided = m.double_sided;
            v.unlit = m.unlit;
            validateMaterial(v);
            const cgltf_texture_view* views[]{&m.pbr_metallic_roughness.base_color_texture,
                                              &m.pbr_metallic_roughness.metallic_roughness_texture, &m.normal_texture,
                                              &m.occlusion_texture, &m.emissive_texture};
            for (size_t s = 0; s < 5; ++s)
                mat->textures[s] = texture(
                    *views[s], "/materials/" + std::to_string(i) + "/texture/" + std::to_string(s), s == 0 || s == 4,
                    s == 2, s == 0 && v.mask ? v.alphaCutoff / std::max(v.baseColor.a, .00001f) : -1);
        }
        bundle->materials.push_back(mat);
    }
    for (size_t mi = 0; mi < d->meshes_count; ++mi) {
        const auto& m = d->meshes[mi];
        auto mesh = std::make_shared<MeshAsset>();
        mesh->id = meta.sub("/meshes/" + std::to_string(mi));
        mesh->name = m.name ? m.name : "Mesh " + std::to_string(mi);
        mesh->revision = content;
        mesh->bounds = {glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX)};
        for (size_t pi = 0; pi < m.primitives_count; ++pi) {
            const auto& p = m.primitives[pi];
            if (p.type != cgltf_primitive_type_triangles || p.targets_count)
                throw std::runtime_error("Only static TRIANGLES primitives are supported");
            std::array<const cgltf_accessor*, 6> a{};
            for (size_t k = 0; k < p.attributes_count; ++k) {
                const auto& at = p.attributes[k];
                if (at.type == cgltf_attribute_type_position)
                    a[0] = at.data;
                if (at.type == cgltf_attribute_type_normal)
                    a[1] = at.data;
                if (at.type == cgltf_attribute_type_tangent)
                    a[2] = at.data;
                if (at.type == cgltf_attribute_type_texcoord && at.index >= 0 && at.index < 2)
                    a[3 + at.index] = at.data;
                if (at.type == cgltf_attribute_type_color && at.index == 0)
                    a[5] = at.data;
            }
            if (!a[0] || a[0]->count > 4000000)
                throw std::runtime_error("Missing/oversized POSITION accessor");
            const auto count = a[0]->count;
            auto pos = floats(a[0], count, 3), norm = floats(a[1], count, 3), tan = floats(a[2], count, 4),
                 uv0 = floats(a[3], count, 2), uv1 = floats(a[4], count, 2);
            auto colors = floats(a[5], count, a[5] ? static_cast<int>(cgltf_num_components(a[5]->type)) : 4);
            std::vector<AssetVertex> original(count);
            for (size_t i = 0; i < count; ++i) {
                auto& v = original[i];
                std::memcpy(&v.position, pos.data() + i * 3, 12);
                if (a[1]) {
                    std::memcpy(&v.normal, norm.data() + i * 3, 12);
                    if (glm::length(v.normal) < 1e-8f)
                        throw std::runtime_error("Zero normal");
                    v.normal = glm::normalize(v.normal);
                } else
                    v.normal = glm::vec3(0);
                if (a[2])
                    std::memcpy(&v.tangent, tan.data() + i * 4, 16);
                if (a[3])
                    std::memcpy(&v.uv0, uv0.data() + i * 2, 8);
                if (a[4])
                    std::memcpy(&v.uv1, uv1.data() + i * 2, 8);
                if (a[5]) {
                    const auto n = cgltf_num_components(a[5]->type);
                    if (n != 3 && n != 4)
                        throw std::runtime_error("Invalid COLOR_0");
                    std::memcpy(&v.color, colors.data() + i * n, n * 4);
                }
            }
            const size_t indexCount = p.indices ? p.indices->count : count;
            if (indexCount % 3 || indexCount > 12000000)
                throw std::runtime_error("Invalid triangle index count");
            std::vector<uint32_t> indices(indexCount);
            if (p.indices) {
                if (cgltf_accessor_unpack_indices(p.indices, indices.data(), 4, indexCount) != indexCount)
                    throw std::runtime_error("Unsupported/sparse index accessor");
            } else
                for (size_t i = 0; i < count; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            for (auto index : indices)
                if (index >= count)
                    throw std::runtime_error("Mesh index outside vertex buffer");
            if (!a[1]) {
                for (size_t i = 0; i < indices.size(); i += 3) {
                    auto& v0 = original[indices[i]];
                    auto& v1 = original[indices[i + 1]];
                    auto& v2 = original[indices[i + 2]];
                    const auto n = glm::cross(v1.position - v0.position, v2.position - v0.position);
                    v0.normal += n;
                    v1.normal += n;
                    v2.normal += n;
                }
                for (auto& v : original)
                    v.normal = glm::length(v.normal) > 1e-8f ? glm::normalize(v.normal) : glm::vec3(0, 1, 0);
            }
            const auto matIndex = p.material ? static_cast<size_t>(p.material - d->materials) : d->materials_count;
            const auto& mat = *bundle->materials[matIndex];
            for (auto& s : mat.textures)
                if (s.texture && !a[3 + s.texCoord])
                    throw std::runtime_error("Material references a missing UV set");
            std::vector<AssetVertex> expanded;
            expanded.reserve(indices.size());
            for (auto i : indices)
                expanded.push_back(original[i]);
            if (!a[2]) {
                for (auto& vertex : expanded) {
                    auto axis = std::abs(vertex.normal.y) < .9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    vertex.tangent = glm::vec4(glm::normalize(glm::cross(axis, vertex.normal)), 1);
                }
                if (mat.textures[2].texture)
                    tangents(expanded, mat.textures[2].texCoord);
            } else {
                for (auto& vertex : expanded) {
                    const auto tangent =
                        glm::vec3(vertex.tangent) - vertex.normal * glm::dot(vertex.normal, glm::vec3(vertex.tangent));
                    if (glm::length(tangent) < 1e-6f || std::abs(std::abs(vertex.tangent.w) - 1) > .001f)
                        throw std::runtime_error("Invalid tangent frame");
                    vertex.tangent = glm::vec4(glm::normalize(tangent), vertex.tangent.w < 0 ? -1.0f : 1.0f);
                }
            }
            MeshPart part{static_cast<uint32_t>(mesh->indices.size()), static_cast<uint32_t>(indices.size()), mat.id};
            std::unordered_map<std::string, uint32_t> unique;
            for (const auto& v : expanded) {
                std::string key(reinterpret_cast<const char*>(&v), sizeof(v));
                auto [it, inserted] = unique.try_emplace(key, static_cast<uint32_t>(mesh->vertices.size()));
                if (inserted) {
                    mesh->vertices.push_back(v);
                    mesh->bounds.min = glm::min(mesh->bounds.min, v.position);
                    mesh->bounds.max = glm::max(mesh->bounds.max, v.position);
                }
                mesh->indices.push_back(it->second);
            }
            mesh->parts.push_back(part);
        }
        if (mesh->indices.empty())
            throw std::runtime_error("Empty mesh");
        buildMeshBvh(*mesh);
        bundle->meshes.push_back(mesh);
    }
    std::set<const cgltf_node*> visited;
    std::function<void(cgltf_node*, int, int)> node = [&](cgltf_node* n, int parent, int depth) {
        if (depth > 256 || !visited.insert(n).second)
            throw std::runtime_error("Cyclic/repeated glTF node or excessive hierarchy");
        ModelNode out;
        out.parent = parent;
        out.name = n->name ? n->name : "Node";
        cgltf_node_transform_local(n, &out.local[0][0]);
        decomposeTrs(out.local);
        if (n->mesh)
            out.mesh = bundle->meshes[static_cast<size_t>(n->mesh - d->meshes)]->id;
        const int index = static_cast<int>(bundle->nodes.size());
        bundle->nodes.push_back(out);
        for (size_t i = 0; i < n->children_count; ++i)
            node(n->children[i], index, depth + 1);
    };
    auto* selected = d->scene ? d->scene : (d->scenes_count ? d->scenes : nullptr);
    if (selected) {
        for (size_t i = 0; i < selected->nodes_count; ++i)
            node(selected->nodes[i], -1, 0);
    } else
        for (size_t i = 0; i < d->nodes_count; ++i)
            if (!d->nodes[i].parent)
                node(&d->nodes[i], -1, 0);
    if (bundle->nodes.empty() || bundle->meshes.empty())
        throw std::runtime_error("Model has no supported scene geometry");
    if (!importing && meta.createdSubAsset)
        throw std::runtime_error("Model structure changed: fresh import required to assign UUIDs");
    std::filesystem::path installed = source, stage;
    bool committed = false, ownsStage = false;
    const auto stageParent = std::filesystem::weakly_canonical(root_) / ".proto";
    const auto stageName = "import-" + meta.id.string();
    struct Cleanup {
        std::filesystem::path& stage;
        const std::filesystem::path& parent;
        const std::string& name;
        bool& done;
        bool& owned;
        ~Cleanup() {
            if (done || !owned)
                return;
            std::error_code error;
            const auto resolved = std::filesystem::weakly_canonical(stage, error);
            if (!error && resolved.parent_path() == parent && resolved.filename() == name)
                std::filesystem::remove_all(resolved, error);
        }
    } cleanup{stage, stageParent, stageName, committed, ownsStage};
    if (importing) {
        const auto folder =
            source.stem().wstring() + L"-" + std::filesystem::path(meta.id.string().substr(0, 8)).wstring();
        installed = root_ / "Assets" / folder / source.filename();
        stage = resourcePath(root_, ".proto/" + stageName);
        if (stage.parent_path() != stageParent || std::filesystem::exists(stage) ||
            std::filesystem::exists(installed.parent_path()))
            throw std::runtime_error("Import destination exists or is redirected");
        std::filesystem::create_directories(stage.parent_path());
        ownsStage = std::filesystem::create_directory(stage);
        if (!ownsStage)
            throw std::runtime_error("Import staging collision");
        {
            std::ofstream output(stage / source.filename(), std::ios::binary);
            output.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (!output)
                throw std::runtime_error("Cannot copy model source");
        }
        for (auto& [uri, bytes] : external) {
            const auto input = assetUri(source.parent_path(), uri);
            if (input == std::filesystem::weakly_canonical(source))
                throw std::runtime_error("Dependency collides with model source");
            const auto path = input.lexically_relative(std::filesystem::weakly_canonical(source.parent_path()));
            const auto target = resourcePath(stage, utf8(path.generic_wstring()));
            std::filesystem::create_directories(target.parent_path());
            std::ofstream output(target, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!output)
                throw std::runtime_error("Cannot copy model dependency");
        }
    }
    bundle->source = utf8(installed.lexically_relative(root_).generic_wstring());
    meta.signature = signature;
    meta.content = content;
    std::filesystem::create_directories(cacheFile.parent_path());
    saveCooked(cacheFile, *bundle);
    meta.cacheHash = sha256(assetBytes(cacheFile, 512 * 1024 * 1024));
    atomicWrite(cacheIntegrity, meta.cacheHash + "\n");
    const auto metadata = emitMeta(meta, deps);
    if (importing) {
        auto target = stage / source.filename();
        target += L".meta";
        atomicWrite(target, metadata);
        std::filesystem::create_directories(installed.parent_path().parent_path());
        std::filesystem::rename(stage, installed.parent_path());
        committed = true;
        rebuildRegistry();
    }
    overrides(*bundle, meta);
    return bundle;
}
PreparedMaterialEdit AssetWorkspace::prepareMaterial(const MaterialAsset& material) {
    validateMaterial(material.values);
    std::string relative;
    for (auto& [id, source] : scanRegistry(root_))
        if (id == material.owner) {
            relative = source + ".meta";
        }
    if (relative.empty())
        throw std::runtime_error("Material owner missing");
    auto path = resourcePath(root_, relative);
    auto meta = readMeta(path);
    JsonDoc old(meta.overrides);
    std::ostringstream values;
    const auto& v = material.values;
    values << std::setprecision(9) << "{\"baseColor\":[" << v.baseColor.r << ',' << v.baseColor.g << ','
           << v.baseColor.b << ',' << v.baseColor.a << "],\"metallic\":" << v.metallic
           << ",\"roughness\":" << v.roughness << ",\"normalScale\":" << v.normalScale << '}';
    std::ostringstream updated;
    updated << '{';
    bool comma = false;
    yyjson_obj_iter it = yyjson_obj_iter_with(old.root());
    while (auto* k = yyjson_obj_iter_next(&it)) {
        if (str(k) == material.id.string())
            continue;
        char* value = yyjson_val_write(yyjson_obj_iter_get_val(k), 0, nullptr);
        if (comma)
            updated << ',';
        comma = true;
        updated << jsonString(str(k)) << ':' << value;
        free(value);
    }
    if (comma)
        updated << ',';
    updated << jsonString(material.id.string()) << ':' << values.str() << '}';
    meta.overrides = updated.str();
    return {relative, meta.previous, emitMeta(meta, meta.dependencies)};
}
void AssetWorkspace::saveMaterial(const MaterialAsset& material) {
    const auto prepared = prepareMaterial(material);
    atomicWrite(resourcePath(root_, prepared.path), prepared.after, prepared.before);
}
void ImportJob::start(std::shared_ptr<AssetWorkspace> workspace, std::filesystem::path source) {
    if (busy())
        throw std::runtime_error("Import already running");
    future_ = std::async(std::launch::async, [workspace, source] { return workspace->importFile(source); });
}
void ImportJob::reload(std::shared_ptr<AssetWorkspace> workspace, AssetId source) {
    if (busy())
        throw std::runtime_error("Import already running");
    future_ = std::async(std::launch::async, [workspace, source] { return workspace->load(source); });
}
std::shared_ptr<const ModelBundle> ImportJob::take() {
    if (!busy() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return {};
    return future_.get();
}
} // namespace proto
