#include "project/AssetFilePlanner.hpp"

#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include <yyjson.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace proto {
namespace {
using Path = std::filesystem::path;

constexpr size_t maxJsonBytes = 256 * 1024 * 1024;

std::string lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return value;
}

std::string relString(const Path& root, const Path& path) {
    return projectRelative(root, path);
}

Path rootPath(const Path& root) {
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(root, error);
    if (error)
        throw std::runtime_error("Cannot resolve project root");
    return result;
}

Path authoredPath(const Path& root, const std::string& relative) {
    const auto value = utf8Path(relative);
    if (value.empty() || value.is_absolute() || value.has_root_name())
        throw std::runtime_error("Project path must be relative");
    const auto base = rootPath(root);
    const auto result = std::filesystem::weakly_canonical(base / value);
    const auto relativeResult = result.lexically_relative(base);
    if (relativeResult.empty() || relativeResult == "." || *relativeResult.begin() == "..")
        throw std::runtime_error("Project path escapes its root");
    return result;
}

bool isMetaPath(const std::string& relative) {
    return lower(relative).ends_with(".meta");
}

bool isBackupOrLock(const std::string& relative) {
    const auto name = lower(utf8(utf8Path(relative).filename().wstring()));
    return name.ends_with(".bak") || name.ends_with(".lock") || name.find(".tmp-") != std::string::npos;
}

bool isServicePath(const std::string& relative) {
    const auto key = lower(projectPathKey(relative));
    if (key == ".proto" || key.starts_with(".proto/"))
        return true;
    return isBackupOrLock(relative);
}

bool isProtectedRoot(const std::string& relative) {
    const auto value = utf8Path(relative);
    if (value.has_parent_path())
        return false;
    const auto name = lower(utf8(value.filename().wstring()));
    return name == "assets" || name == "scenes" || name == "code" || name == "config" || name == "project.proto.json";
}

void rejectBrowserPath(const std::string& relative, bool allowMeta = false) {
    if (relative.empty() || isServicePath(relative) || isProtectedRoot(relative) ||
        (!allowMeta && isMetaPath(relative)))
        throw std::runtime_error("Project control or metadata paths are not browser-operation targets");
}

std::string normalizeInputPath(const Path& root, const std::string& input, bool allowMeta = false) {
    if (input.empty())
        throw std::runtime_error("Empty project path");
    const auto path = projectPath(root, input, allowMeta);
    auto result = relString(rootPath(root), path);
    if (result.empty() || result == ".")
        throw std::runtime_error("The project root is not an authored file target");
    rejectBrowserPath(result, allowMeta);
    return result;
}

bool sameOrDescendant(const std::string& parent, const std::string& child) {
    const auto p = projectPathKey(parent);
    const auto c = projectPathKey(child);
    return c == p || (c.size() > p.size() && c.starts_with(p) && c[p.size()] == '/');
}

std::string appendPath(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory == ".")
        return name;
    return directory + "/" + name;
}

std::string filename(const std::string& relative) {
    return utf8(utf8Path(relative).filename().wstring());
}

std::string dirname(const std::string& relative) {
    const auto value = utf8Path(relative).parent_path();
    return value.empty() ? std::string{} : utf8(value.generic_wstring());
}

std::string removeMetaSuffix(const std::string& relative) {
    if (!isMetaPath(relative))
        return relative;
    return relative.substr(0, relative.size() - 5);
}

std::string sidecarPath(const std::string& relative) {
    return relative + ".meta";
}

std::string extension(const std::string& relative) {
    return lower(utf8(utf8Path(relative).extension().wstring()));
}

bool isModelPath(const std::string& relative) {
    const auto value = extension(relative);
    return value == ".gltf" || value == ".glb";
}

bool isScenePath(const std::string& relative) {
    return lower(relative).ends_with(".scene.json");
}

std::string readFile(const Path& root, const std::string& relative) {
    const auto bytes = assetBytes(authoredPath(root, relative), maxJsonBytes);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct JsonDocument {
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> value{nullptr, yyjson_doc_free};
    explicit JsonDocument(const std::string& bytes) {
        yyjson_read_err error{};
        value.reset(yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(), 0, nullptr, &error));
        if (!value)
            throw std::runtime_error(std::string("Project JSON: ") + error.msg);
    }
    JsonDocument(JsonDocument&&) noexcept = default;
    JsonDocument& operator=(JsonDocument&&) noexcept = default;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    Json* root() const { return yyjson_doc_get_root(value.get()); }
};

std::string jsonText(Json* value, const char* field) {
    if (!value || !yyjson_is_str(value))
        throw std::runtime_error(std::string("Project JSON field is not a string: ") + field);
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

std::optional<std::string> optionalJsonText(Json* object, const char* field) {
    if (auto* value = yyjson_obj_get(object, field))
        return jsonText(value, field);
    return std::nullopt;
}

Json* jsonField(Json* object, const char* field) {
    auto* value = yyjson_obj_get(object, field);
    if (!value)
        throw std::runtime_error(std::string("Project JSON field missing: ") + field);
    return value;
}

bool jsonObject(Json* value) {
    return value && yyjson_is_obj(value);
}

std::string writeJson(yyjson_mut_doc* document) {
    size_t length{};
    std::unique_ptr<char, decltype(&free)> data(
        yyjson_mut_write(document, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length), free);
    if (!data || length > maxJsonBytes)
        throw std::runtime_error("Project JSON output is too large");
    return {data.get(), length};
}

template <class Mutator> std::string mutateJson(const std::string& bytes, Mutator&& mutator) {
    JsonDocument source(bytes);
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document(
        yyjson_doc_mut_copy(source.value.get(), nullptr), yyjson_mut_doc_free);
    if (!document)
        throw std::bad_alloc();
    mutator(document.get(), yyjson_mut_doc_get_root(document.get()));
    return writeJson(document.get());
}

void setJsonString(yyjson_mut_doc* document, const std::string& pointer, const std::string& value) {
    auto* replacement = yyjson_mut_strcpy(document, value.c_str());
    if (!replacement || !yyjson_mut_doc_ptr_set(document, pointer.c_str(), replacement))
        throw std::runtime_error("Cannot update project JSON pointer: " + pointer);
}

std::optional<std::string> mutableString(yyjson_mut_val* value) {
    if (!value || !yyjson_mut_is_str(value))
        return std::nullopt;
    return std::string(yyjson_get_str(reinterpret_cast<Json*>(value)), yyjson_get_len(reinterpret_cast<Json*>(value)));
}

uint32_t le32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated GLB header");
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
}

void putLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Invalid GLB output");
    bytes[offset] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
    bytes[offset + 2] = uint8_t(value >> 16);
    bytes[offset + 3] = uint8_t(value >> 24);
}

struct ModelJson {
    bool glb{};
    std::string json;
    std::vector<uint8_t> raw;
};

ModelJson modelJson(const std::string& bytes, bool glb) {
    if (!glb)
        return {false, bytes, {}};
    std::vector<uint8_t> raw(bytes.begin(), bytes.end());
    if (raw.size() < 20 || le32(raw, 0) != 0x46546c67u || le32(raw, 4) != 2 || le32(raw, 8) != raw.size())
        throw std::runtime_error("Invalid GLB container");
    const auto jsonSize = le32(raw, 12);
    if (le32(raw, 16) != 0x4e4f534au || jsonSize > raw.size() - 20)
        throw std::runtime_error("GLB JSON chunk is missing");
    std::string json(reinterpret_cast<const char*>(raw.data() + 20), jsonSize);
    while (!json.empty() && (json.back() == '\0' || json.back() == ' ' || json.back() == '\n' || json.back() == '\r' ||
                             json.back() == '\t'))
        json.pop_back();
    return {true, std::move(json), std::move(raw)};
}

std::string rebuildGlb(const std::vector<uint8_t>& original, const std::string& json) {
    if (original.size() < 20 || le32(original, 0) != 0x46546c67u || le32(original, 4) != 2)
        throw std::runtime_error("Invalid GLB container");
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
    size_t offset = 12;
    bool jsonSeen = false;
    while (offset < original.size()) {
        if (original.size() - offset < 8)
            throw std::runtime_error("Truncated GLB chunk");
        const auto size = le32(original, offset);
        const auto type = le32(original, offset + 4);
        offset += 8;
        if (size > original.size() - offset)
            throw std::runtime_error("GLB chunk exceeds container");
        std::vector<uint8_t> payload(original.begin() + static_cast<std::ptrdiff_t>(offset),
                                     original.begin() + static_cast<std::ptrdiff_t>(offset + size));
        if (type == 0x4e4f534au && !jsonSeen) {
            payload.assign(json.begin(), json.end());
            while (payload.size() % 4)
                payload.push_back(' ');
            jsonSeen = true;
        }
        chunks.emplace_back(type, std::move(payload));
        offset += size;
    }
    if (!jsonSeen)
        throw std::runtime_error("GLB JSON chunk is missing");
    std::vector<uint8_t> output(12);
    putLe32(output, 0, 0x46546c67u);
    putLe32(output, 4, 2);
    for (const auto& [type, payload] : chunks) {
        const auto oldSize = output.size();
        output.resize(oldSize + 8 + payload.size());
        putLe32(output, oldSize, static_cast<uint32_t>(payload.size()));
        putLe32(output, oldSize + 4, type);
        std::copy(payload.begin(), payload.end(), output.begin() + static_cast<std::ptrdiff_t>(oldSize + 8));
    }
    if (output.size() > UINT32_MAX)
        throw std::runtime_error("GLB output is too large");
    putLe32(output, 8, static_cast<uint32_t>(output.size()));
    return {reinterpret_cast<const char*>(output.data()), output.size()};
}

struct UriReference {
    std::string uri;
    std::string pointer;
    std::string path;
};

std::vector<UriReference> modelUris(const Path& root, const std::string& relative) {
    const auto bytes = readFile(root, relative);
    const auto parsed = modelJson(bytes, extension(relative) == ".glb");
    JsonDocument document(parsed.json);
    auto* rootValue = document.root();
    std::vector<UriReference> result;
    for (const char* collection : {"buffers", "images"}) {
        auto* array = yyjson_obj_get(rootValue, collection);
        if (!array)
            continue;
        if (!yyjson_is_arr(array))
            throw std::runtime_error(std::string("glTF ") + collection + " is not an array");
        const auto count = yyjson_arr_size(array);
        for (size_t i = 0; i < count; ++i) {
            auto* item = yyjson_arr_get(array, i);
            if (!jsonObject(item))
                throw std::runtime_error("glTF external-resource entry is not an object");
            auto* uriValue = yyjson_obj_get(item, "uri");
            if (!uriValue)
                continue;
            const auto uri = jsonText(uriValue, "uri");
            if (uri.starts_with("data:"))
                continue;
            const auto resolved =
                assetUriWithin(authoredPath(root, relative).parent_path(), uri, rootPath(root) / "Assets");
            const auto dependency = relString(rootPath(root), resolved);
            result.push_back({uri, "/" + std::string(collection) + "/" + std::to_string(i) + "/uri", dependency});
        }
    }
    return result;
}

std::string encodeUriPath(const std::string& relative) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : relative) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (unreserved)
            result.push_back(static_cast<char>(c));
        else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        }
    }
    return result;
}

bool pathWithin(const Path& base, const Path& value) {
    const auto relative = value.lexically_relative(base);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

std::string relativeUri(const Path& root, const std::string& model, const std::string& dependency) {
    const auto from = dirname(model).empty() ? rootPath(root) : authoredPath(root, dirname(model));
    const auto to = authoredPath(root, dependency);
    const auto relative = to.lexically_relative(from);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("Rewritten glTF URI leaves the Assets root");
    const auto assetsRoot = std::filesystem::weakly_canonical(rootPath(root) / "Assets");
    const auto resolved = std::filesystem::weakly_canonical(from / relative);
    if (!pathWithin(assetsRoot, resolved))
        throw std::runtime_error("Rewritten glTF URI leaves the Assets root");
    return encodeUriPath(utf8(relative.generic_wstring()));
}

std::string rewriteModelBytes(const std::string& bytes, bool glb, const std::map<std::string, std::string>& updates) {
    if (updates.empty())
        return bytes;
    const auto parsed = modelJson(bytes, glb);
    const auto updatedJson = mutateJson(parsed.json, [&](yyjson_mut_doc* document, yyjson_mut_val*) {
        for (const auto& [pointer, value] : updates)
            setJsonString(document, pointer, value);
    });
    return glb ? rebuildGlb(parsed.raw, updatedJson) : updatedJson;
}

struct DependencyRecord {
    std::string uri;
    std::string pointer;
    std::string path;
    std::string assetId;
    std::string hash;
};

struct MetadataRecord {
    std::string sourcePath;
    std::string metaPath;
    std::string kind;
    std::string id;
    std::string owner;
    std::optional<std::string> declaredSourcePath;
    std::string bytes;
    std::vector<std::pair<std::string, std::string>> subAssets;
    std::vector<DependencyRecord> dependencies;
};

struct SceneRecord {
    std::string path;
    std::string metaPath;
    std::string id;
    std::string bytes;
};

struct ProjectIndex {
    Path root;
    Path assets;
    std::vector<std::string> files;
    std::vector<std::string> directories;
    std::map<std::string, MetadataRecord> metadata;
    std::map<std::string, SceneRecord> scenes;
    std::map<std::string, std::vector<UriReference>> modelUris;
    std::map<std::string, std::string> idToPath;
    std::map<std::string, std::string> idToKind;
    std::map<std::string, std::string> idToOwner;
};

void addAssetId(ProjectIndex& index, const std::string& id, const std::string& path, const std::string& kind,
                const std::string& owner = {}) {
    if (id.empty())
        return;
    const auto key = lower(id);
    auto [it, inserted] = index.idToPath.emplace(key, path);
    if (!inserted && it->second != path)
        throw std::runtime_error("Duplicate project AssetId: " + id);
    index.idToKind[key] = kind;
    index.idToOwner[key] = owner;
}

std::string metaKind(Json* rootValue) {
    if (!rootValue || !yyjson_is_obj(rootValue))
        throw std::runtime_error("Asset sidecar must contain an object");
    return jsonText(jsonField(rootValue, "kind"), "kind");
}

MetadataRecord parseMetadata(const Path& root, const std::string& sourcePath, const std::string& metaPath) {
    MetadataRecord record;
    record.sourcePath = sourcePath;
    record.metaPath = metaPath;
    record.bytes = readFile(root, metaPath);
    JsonDocument document(record.bytes);
    auto* rootValue = document.root();
    const auto format = optionalJsonText(rootValue, "format");
    const auto version = yyjson_obj_get(rootValue, "formatVersion");
    if (format && *format != "proto.assetmeta")
        throw std::runtime_error("Unsupported project sidecar format");
    if (version && (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1))
        throw std::runtime_error("Unsupported project sidecar version");
    record.kind = metaKind(rootValue);
    if (auto id = optionalJsonText(rootValue, "assetId")) {
        AssetId::parse(*id);
        record.id = *id;
    }
    if (auto owner = optionalJsonText(rootValue, "owner")) {
        AssetId::parse(*owner);
        record.owner = *owner;
    }
    record.declaredSourcePath = optionalJsonText(rootValue, "sourcePath");
    if (record.kind == "ModelSource") {
        if (record.id.empty() || !yyjson_is_arr(jsonField(rootValue, "subAssets")) ||
            !yyjson_is_arr(jsonField(rootValue, "dependencies")))
            throw std::runtime_error("ModelSource sidecar is incomplete");
        size_t i, n;
        Json* value;
        yyjson_arr_foreach(jsonField(rootValue, "subAssets"), i, n, value) {
            const auto id = jsonText(jsonField(value, "id"), "id");
            AssetId::parse(id);
            const auto locator = jsonText(jsonField(value, "locator"), "locator");
            record.subAssets.emplace_back(id, locator);
        }
        yyjson_arr_foreach(jsonField(rootValue, "dependencies"), i, n, value) {
            DependencyRecord dependency;
            dependency.uri = jsonText(jsonField(value, "uri"), "uri");
            dependency.hash = jsonText(jsonField(value, "sha256"), "sha256");
            if (auto id = optionalJsonText(value, "assetId")) {
                AssetId::parse(*id);
                dependency.assetId = *id;
            }
            if (auto pointer = optionalJsonText(value, "pointer"))
                dependency.pointer = *pointer;
            else if (auto pointer = optionalJsonText(value, "jsonPointer"))
                dependency.pointer = *pointer;
            record.dependencies.push_back(std::move(dependency));
        }
    }
    return record;
}

std::string sceneId(const std::string& bytes) {
    JsonDocument document(bytes);
    const auto id = jsonText(jsonField(document.root(), "sceneId"), "sceneId");
    AssetId::parse(id);
    return id;
}

void collectTree(const Path& root, const Path& directory, std::vector<std::string>& files,
                 std::vector<std::string>& directories) {
    if (!std::filesystem::exists(directory))
        return;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end; it.increment(error)) {
        if (error)
            throw std::runtime_error("Cannot scan project authored files");
        if (it->is_symlink(error) || error)
            throw std::runtime_error("Project authored tree contains a reparse or symbolic link");
        const auto relative = relString(root, it->path());
        if (isServicePath(relative)) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(error))
            directories.push_back(relative);
        else if (it->is_regular_file(error))
            files.push_back(relative);
        else
            throw std::runtime_error("Project authored tree contains a reparse or unsupported entry");
    }
    std::sort(files.begin(), files.end());
    std::sort(directories.begin(), directories.end());
}

ProjectIndex indexProject(const Path& projectRoot) {
    ProjectIndex index;
    index.root = rootPath(projectRoot);
    index.assets = index.root / "Assets";
    for (const auto* name : {"Assets", "Scenes", "Code", "Config"}) {
        const auto top = index.root / name;
        const auto status = std::filesystem::symlink_status(top);
        if (status.type() == std::filesystem::file_type::not_found)
            continue;
        if (std::filesystem::is_symlink(status))
            throw std::runtime_error("Project authored root contains a reparse or symbolic link: " + std::string(name));
        if (!std::filesystem::is_directory(status))
            throw std::runtime_error("Project authored root is not a directory: " + std::string(name));
        index.directories.emplace_back(name);
        collectTree(index.root, top, index.files, index.directories);
    }
    std::sort(index.files.begin(), index.files.end());
    index.files.erase(std::unique(index.files.begin(), index.files.end()), index.files.end());
    std::sort(index.directories.begin(), index.directories.end());
    index.directories.erase(std::unique(index.directories.begin(), index.directories.end()), index.directories.end());

    std::map<std::string, std::string> sourceToMeta;
    for (const auto& file : index.files)
        if (isMetaPath(file))
            sourceToMeta.emplace(removeMetaSuffix(file), file);

    for (const auto& [source, meta] : sourceToMeta) {
        if (!std::filesystem::is_regular_file(authoredPath(index.root, source)))
            throw std::runtime_error("Metadata has no authored source: " + meta);
        const auto parsed = parseMetadata(index.root, source, meta);
        if (parsed.kind == "ModelSource") {
            if (!isModelPath(source))
                throw std::runtime_error("ModelSource metadata is attached to a non-model source");
            std::set<std::string> localIds;
            if (!localIds.insert(lower(parsed.id)).second)
                throw std::runtime_error("Duplicate ModelSource UUID: " + parsed.id);
            for (const auto& [id, locator] : parsed.subAssets)
                if (!localIds.insert(lower(id)).second)
                    throw std::runtime_error("Duplicate ModelSource subasset UUID: " + id);
            index.metadata.emplace(source, parsed);
            addAssetId(index, parsed.id, source, "ModelSource");
            for (const auto& [id, locator] : parsed.subAssets) {
                const auto kind = locator.starts_with("/meshes/") ? "Mesh"
                                  : locator.starts_with("/materials/") && locator.find("/texture/") == std::string::npos
                                      ? "Material"
                                      : "Texture";
                addAssetId(index, id, source, kind, parsed.id);
            }
        } else if (parsed.kind == "Scene") {
            if (!isScenePath(source))
                throw std::runtime_error("Scene metadata is attached to a non-scene source");
            if (!parsed.id.empty())
                addAssetId(index, parsed.id, source, "Scene");
            index.metadata.emplace(source, parsed);
        } else if (parsed.kind == "RawDependency") {
            if (!parsed.id.empty())
                addAssetId(index, parsed.id, source, "RawDependency", parsed.owner);
            index.metadata.emplace(source, parsed);
        } else {
            if (isModelPath(source))
                throw std::runtime_error("Model source sidecar has unsupported kind: " + parsed.kind);
            if (!parsed.id.empty())
                addAssetId(index, parsed.id, source, parsed.kind, parsed.owner);
            index.metadata.emplace(source, parsed);
        }
    }

    for (const auto& file : index.files) {
        if (isMetaPath(file) || !isScenePath(file))
            continue;
        SceneRecord scene{file, sidecarPath(file), sceneId(readFile(index.root, file)), readFile(index.root, file)};
        if (const auto found = index.metadata.find(file); found != index.metadata.end()) {
            if (!found->second.id.empty() && found->second.id != scene.id)
                throw std::runtime_error("Scene sidecar AssetId disagrees with sceneId: " + file);
            scene.metaPath = found->second.metaPath;
        }
        addAssetId(index, scene.id, file, "Scene");
        index.scenes.emplace(file, std::move(scene));
    }

    for (auto& [source, meta] : index.metadata)
        if (meta.kind == "ModelSource") {
            index.modelUris.emplace(source, modelUris(index.root, source));
            auto& refs = index.modelUris.at(source);
            for (auto& ref : refs) {
                auto dependencyMeta = index.metadata.find(ref.path);
                if (dependencyMeta != index.metadata.end() && dependencyMeta->second.kind == "RawDependency")
                    ref.path = ref.path;
                for (auto& dependency : meta.dependencies)
                    if (dependency.uri == ref.uri) {
                        dependency.path = ref.path;
                        if (dependency.assetId.empty() && dependencyMeta != index.metadata.end())
                            dependency.assetId = dependencyMeta->second.id;
                        break;
                    }
            }
            // URI dependencies can exist before their sidecar is enrolled.
            for (const auto& ref : refs)
                if (!std::filesystem::is_regular_file(authoredPath(index.root, ref.path)))
                    throw std::runtime_error("Missing glTF URI dependency: " + ref.path);
        }
    return index;
}

std::vector<ProjectAsset> assetsFromIndex(const ProjectIndex& index) {
    std::vector<ProjectAsset> result;
    for (const auto& [source, meta] : index.metadata) {
        if (!meta.id.empty())
            result.push_back({meta.id, source, meta.kind, meta.owner, source});
        if (meta.kind == "ModelSource")
            for (const auto& [id, locator] : meta.subAssets)
                result.push_back({id, source,
                                  locator.starts_with("/meshes/") ? "Mesh"
                                  : locator.starts_with("/materials/") && locator.find("/texture/") == std::string::npos
                                      ? "Material"
                                      : "Texture",
                                  meta.id, source});
    }
    for (const auto& [source, scene] : index.scenes)
        result.push_back({scene.id, source, "Scene", {}, source});
    std::set<std::string> seen;
    std::vector<ProjectAsset> unique;
    for (auto& asset : result)
        if (seen.insert(asset.id + "\n" + asset.path + "\n" + asset.kind).second)
            unique.push_back(std::move(asset));
    std::sort(unique.begin(), unique.end(), [](const auto& a, const auto& b) {
        return std::tie(a.path, a.kind, a.id) < std::tie(b.path, b.kind, b.id);
    });
    return unique;
}

std::string remapId(const std::map<std::string, std::string>& ids, const std::string& value) {
    if (const auto found = ids.find(value); found != ids.end())
        return found->second;
    if (const auto found = ids.find(lower(value)); found != ids.end())
        return found->second;
    return value;
}

void remapStringPointer(yyjson_mut_doc* document, const std::string& pointer,
                        const std::map<std::string, std::string>& ids) {
    auto* value = yyjson_mut_doc_ptr_get(document, pointer.c_str());
    if (!value || !yyjson_mut_is_str(value))
        return;
    const auto current = mutableString(value);
    if (current && *current != remapId(ids, *current))
        setJsonString(document, pointer, remapId(ids, *current));
}

std::string rewriteMetadata(const MetadataRecord& metadata, const std::map<std::string, std::string>& ids,
                            const std::map<std::string, std::string>& uriUpdates,
                            const std::optional<std::string>& sourcePath) {
    return mutateJson(metadata.bytes, [&](yyjson_mut_doc* document, yyjson_mut_val*) {
        if (!metadata.id.empty())
            remapStringPointer(document, "/assetId", ids);
        if (metadata.kind == "ModelSource") {
            auto* subAssets = yyjson_mut_doc_ptr_get(document, "/subAssets");
            if (subAssets && yyjson_mut_is_arr(subAssets)) {
                const auto count = yyjson_mut_arr_size(subAssets);
                for (size_t i = 0; i < count; ++i)
                    remapStringPointer(document, "/subAssets/" + std::to_string(i) + "/id", ids);
            }
            auto* dependencies = yyjson_mut_doc_ptr_get(document, "/dependencies");
            if (dependencies && yyjson_mut_is_arr(dependencies)) {
                const auto count = yyjson_mut_arr_size(dependencies);
                for (size_t i = 0; i < count; ++i) {
                    const auto prefix = "/dependencies/" + std::to_string(i);
                    auto* uriValue = yyjson_mut_doc_ptr_get(document, (prefix + "/uri").c_str());
                    if (uriValue && yyjson_mut_is_str(uriValue)) {
                        const auto old = mutableString(uriValue);
                        if (old) {
                            if (const auto found = uriUpdates.find(*old); found != uriUpdates.end())
                                setJsonString(document, prefix + "/uri", found->second);
                        }
                    }
                    remapStringPointer(document, prefix + "/assetId", ids);
                }
            }
            auto* overrides = yyjson_mut_doc_ptr_get(document, "/materialOverrides");
            if (overrides && yyjson_mut_is_obj(overrides) && !ids.empty()) {
                auto* replacement = yyjson_mut_obj(document);
                yyjson_mut_obj_iter iterator = yyjson_mut_obj_iter_with(overrides);
                while (auto* key = yyjson_mut_obj_iter_next(&iterator)) {
                    const auto oldKey = mutableString(key).value_or(std::string{});
                    const auto nextKey = remapId(ids, oldKey);
                    auto* value = yyjson_mut_obj_iter_get_val(key);
                    auto* copy = yyjson_mut_val_mut_copy(document, value);
                    auto* keyCopy = yyjson_mut_strcpy(document, nextKey.c_str());
                    if (!keyCopy || !copy || !yyjson_mut_obj_add(replacement, keyCopy, copy))
                        throw std::bad_alloc();
                }
                if (!yyjson_mut_doc_ptr_set(document, "/materialOverrides", replacement))
                    throw std::runtime_error("Cannot remap material overrides");
            }
        } else {
            remapStringPointer(document, "/owner", ids);
            if (sourcePath && yyjson_mut_doc_ptr_get(document, "/sourcePath"))
                setJsonString(document, "/sourcePath", *sourcePath);
        }
        if (sourcePath && yyjson_mut_doc_ptr_get(document, "/sourcePath"))
            setJsonString(document, "/sourcePath", *sourcePath);
    });
}

std::string rewriteScene(const SceneRecord& scene, const std::map<std::string, std::string>& ids,
                         const std::string& assetRoot, const std::optional<std::string>& copiedId) {
    return mutateJson(scene.bytes, [&](yyjson_mut_doc* document, yyjson_mut_val*) {
        if (copiedId)
            setJsonString(document, "/sceneId", *copiedId);
        if (yyjson_mut_doc_ptr_get(document, "/assetRoot"))
            setJsonString(document, "/assetRoot", assetRoot);

        auto* modelSources = yyjson_mut_doc_ptr_get(document, "/modelSources");
        if (modelSources && yyjson_mut_is_arr(modelSources)) {
            const auto count = yyjson_mut_arr_size(modelSources);
            for (size_t i = 0; i < count; ++i)
                remapStringPointer(document, "/modelSources/" + std::to_string(i), ids);
        }
        auto* entities = yyjson_mut_doc_ptr_get(document, "/entities");
        if (!entities || !yyjson_mut_is_arr(entities))
            return;
        const auto count = yyjson_mut_arr_size(entities);
        for (size_t i = 0; i < count; ++i) {
            const auto base = "/entities/" + std::to_string(i) + "/components/meshRenderer";
            remapStringPointer(document, base + "/mesh", ids);
            auto* materials = yyjson_mut_doc_ptr_get(document, (base + "/materials").c_str());
            if (materials && yyjson_mut_is_arr(materials)) {
                const auto materialCount = yyjson_mut_arr_size(materials);
                for (size_t slot = 0; slot < materialCount; ++slot)
                    remapStringPointer(document, base + "/materials/" + std::to_string(slot), ids);
            }
        }
        // Behavior property names and unknown behavior types are intentionally
        // opaque. Only the explicit {"assetRef": UUID-or-null} wrapper is a
        // typed dependency and may be remapped.
        for (size_t i = 0; i < count; ++i) {
            auto* entity = yyjson_mut_arr_get(entities, i);
            auto* behaviors = entity ? yyjson_mut_obj_get(entity, "behaviors") : nullptr;
            if (!behaviors || !yyjson_mut_is_arr(behaviors))
                continue;
            const auto behaviorCount = yyjson_mut_arr_size(behaviors);
            for (size_t b = 0; b < behaviorCount; ++b) {
                auto* binding = yyjson_mut_arr_get(behaviors, b);
                auto* properties = binding ? yyjson_mut_obj_get(binding, "properties") : nullptr;
                if (!properties || !yyjson_mut_is_obj(properties))
                    continue;
                yyjson_mut_obj_iter propertyIterator = yyjson_mut_obj_iter_with(properties);
                while (auto* propertyKey = yyjson_mut_obj_iter_next(&propertyIterator)) {
                    auto* propertyValue = yyjson_mut_obj_iter_get_val(propertyKey);
                    if (!propertyValue || !yyjson_mut_is_obj(propertyValue))
                        continue;
                    yyjson_mut_obj_iter wrapperIterator = yyjson_mut_obj_iter_with(propertyValue);
                    auto* wrapperKey = yyjson_mut_obj_iter_next(&wrapperIterator);
                    if (!wrapperKey || yyjson_mut_obj_iter_next(&wrapperIterator))
                        continue;
                    const std::string wrapperName(yyjson_mut_get_str(wrapperKey), yyjson_mut_get_len(wrapperKey));
                    if (wrapperName != "assetRef")
                        continue;
                    auto* assetValue = yyjson_mut_obj_iter_get_val(wrapperKey);
                    if (!assetValue || !yyjson_mut_is_str(assetValue))
                        continue;
                    const std::string oldId(yyjson_mut_get_str(assetValue), yyjson_mut_get_len(assetValue));
                    const auto nextId = remapId(ids, oldId);
                    if (nextId != oldId) {
                        // yyjson_mut_set_strn stores a borrowed pointer. The
                        // remapped ID is a local string, so make a document
                        // owned copy before replacing the wrapper value.
                        auto* replacement = yyjson_mut_strcpy(document, nextId.c_str());
                        if (!replacement || !yyjson_mut_obj_replace(propertyValue, wrapperKey, replacement))
                            throw std::runtime_error("Cannot remap behavior AssetRef");
                    }
                }
            }
        }
    });
}

std::string sceneAssetRoot(const Path& root, const std::string& scenePath) {
    const auto sceneParent = dirname(scenePath).empty() ? rootPath(root) : authoredPath(root, dirname(scenePath));
    const auto relative = rootPath(root).lexically_relative(sceneParent);
    const auto resolved = (sceneParent / relative).lexically_normal();
    std::error_code error;
    const bool sameRoot = std::filesystem::equivalent(resolved, rootPath(root), error);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || !sameRoot || error)
        throw std::runtime_error("Scene assetRoot cannot be represented relative to its new location");
    return utf8(relative.generic_wstring()).empty() ? "." : utf8(relative.generic_wstring());
}

struct SelectedTree {
    std::vector<std::string> roots;
    std::set<std::string> paths;
    std::set<std::string> directories;
};

void addTree(const ProjectIndex& index, const std::string& path, SelectedTree& selected) {
    selected.paths.insert(path);
    const auto absolute = authoredPath(index.root, path);
    if (!std::filesystem::is_directory(absolute))
        return;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(absolute, error), end; it != end; it.increment(error)) {
        if (error || it->is_symlink(error) || error)
            throw std::runtime_error("Selected directory contains a reparse or symbolic link");
        const auto relative = relString(index.root, it->path());
        if (isServicePath(relative))
            throw std::runtime_error("Selected directory contains a service/backup entry: " + relative);
    }
    selected.directories.insert(path);
    for (const auto& file : index.files)
        if (sameOrDescendant(path, file))
            selected.paths.insert(file);
    for (const auto& directory : index.directories)
        if (sameOrDescendant(path, directory))
            selected.directories.insert(directory);
}

SelectedTree selectPaths(const ProjectIndex& index, const std::vector<std::string>& sourcePaths) {
    if (sourcePaths.empty())
        throw std::runtime_error("Select at least one authored project path");
    SelectedTree selected;
    std::vector<std::string> normalized;
    for (const auto& input : sourcePaths) {
        const auto path = normalizeInputPath(index.root, input);
        if (!std::filesystem::exists(authoredPath(index.root, path)))
            throw std::runtime_error("Project path does not exist: " + path);
        normalized.push_back(path);
    }
    std::sort(normalized.begin(), normalized.end(), [](const auto& a, const auto& b) {
        const auto depth = [](const std::string& path) { return std::count(path.begin(), path.end(), '/'); };
        const auto da = depth(a), db = depth(b);
        return da == db ? a < b : da < db;
    });
    for (const auto& path : normalized) {
        bool covered = false;
        for (const auto& root : selected.roots)
            if (sameOrDescendant(root, path)) {
                covered = true;
                break;
            }
        if (covered)
            continue;
        selected.roots.push_back(path);
        addTree(index, path, selected);
    }
    for (const auto& path : selected.roots) {
        if (isMetaPath(path))
            throw std::runtime_error("Select the authored resource; its .meta travels implicitly");
        const auto meta = sidecarPath(path);
        if (std::filesystem::exists(authoredPath(index.root, meta)))
            selected.paths.insert(meta);
    }
    return selected;
}

void addGuard(FilePlan& plan, const Path& root, const std::string& path) {
    if (path.empty())
        return;
    const auto key = projectPathKey(path);
    for (const auto& guard : plan.guards)
        if (projectPathKey(guard.path) == key)
            return;
    plan.guards.push_back({path, projectStamp(root, path)});
}

void addEdit(FilePlan& plan, FileEdit edit) {
    for (auto& current : plan.edits)
        if (current.path == edit.path) {
            if (current.kind != edit.kind)
                throw std::runtime_error("Conflicting edits for project path: " + edit.path);
            if (edit.text)
                current.text = std::move(edit.text), current.source.clear();
            else if (!edit.source.empty() && !current.text)
                current.source = std::move(edit.source);
            return;
        }
    plan.edits.push_back(std::move(edit));
}

void addDirectoryCreates(FilePlan& plan, const Path& root, const std::string& path) {
    auto parent = utf8Path(path).parent_path();
    std::vector<std::string> missing;
    while (!parent.empty()) {
        const auto relative = utf8(parent.generic_wstring());
        if (relative.empty() || relative == ".")
            break;
        if (projectStamp(root, relative).kind == FileKind::Missing)
            missing.push_back(relative);
        parent = parent.parent_path();
    }
    std::reverse(missing.begin(), missing.end());
    for (const auto& directory : missing)
        addEdit(plan, {directory, FileKind::Directory, std::nullopt, {}});
}

void addMissingEdit(FilePlan& plan, const std::string& path) {
    addEdit(plan, {path, FileKind::Missing, std::nullopt, {}});
}

std::string destinationName(const std::string& source, const std::optional<std::string>& newName) {
    if (newName)
        return *newName;
    return filename(source);
}

void ensureName(const std::optional<std::string>& newName) {
    if (!newName)
        return;
    if (newName->empty() || newName->find_first_of("/\\") != std::string::npos || *newName == "." || *newName == "..")
        throw std::runtime_error("Invalid destination name");
    const auto value = utf8Path(*newName);
    if (value.empty() || value.filename() != value)
        throw std::runtime_error("Invalid destination name");
}

std::string sidecarWithIdentity(const std::string& bytes, const std::string& id, const std::string& owner,
                                const std::string& sourcePath) {
    return mutateJson(bytes, [&](yyjson_mut_doc* document, yyjson_mut_val* rootValue) {
        if (yyjson_obj_get(reinterpret_cast<Json*>(rootValue), "assetId"))
            setJsonString(document, "/assetId", id);
        else
            yyjson_mut_obj_add_strcpy(document, rootValue, "assetId", id.c_str());
        if (!owner.empty()) {
            if (yyjson_obj_get(reinterpret_cast<Json*>(rootValue), "owner"))
                setJsonString(document, "/owner", owner);
            else
                yyjson_mut_obj_add_strcpy(document, rootValue, "owner", owner.c_str());
        }
        if (!sourcePath.empty()) {
            if (yyjson_obj_get(reinterpret_cast<Json*>(rootValue), "sourcePath"))
                setJsonString(document, "/sourcePath", sourcePath);
            else
                yyjson_mut_obj_add_strcpy(document, rootValue, "sourcePath", sourcePath.c_str());
        }
    });
}

std::string newRawDependencyMeta(const std::string& id, const std::string& owner, const std::string& sourcePath) {
    std::ostringstream output;
    output << "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"kind\":\"RawDependency\","
           << "\"assetId\":" << jsonString(id);
    if (!owner.empty())
        output << ",\"owner\":" << jsonString(owner);
    if (!sourcePath.empty())
        output << ",\"sourcePath\":" << jsonString(sourcePath);
    output << "}\n";
    return output.str();
}

std::string newSceneMeta(const std::string& id) {
    return "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"kind\":\"Scene\",\"assetId\":" + jsonString(id) +
           "}\n";
}

std::string enrichModelMetadata(const Path& root, const MetadataRecord& metadata, const std::vector<UriReference>& refs,
                                const std::map<std::string, std::string>& dependencyIds) {
    std::map<std::string, UriReference> byUri;
    for (const auto& ref : refs)
        byUri.emplace(ref.uri, ref);
    return mutateJson(metadata.bytes, [&](yyjson_mut_doc* document, yyjson_mut_val*) {
        auto* dependencies = yyjson_mut_doc_ptr_get(document, "/dependencies");
        if (!dependencies || !yyjson_mut_is_arr(dependencies))
            throw std::runtime_error("ModelSource dependencies must be an array");
        std::set<std::string> emitted;
        const auto count = yyjson_mut_arr_size(dependencies);
        for (size_t i = 0; i < count; ++i) {
            const auto prefix = "/dependencies/" + std::to_string(i);
            auto* uriValue = yyjson_mut_doc_ptr_get(document, (prefix + "/uri").c_str());
            if (!uriValue || !yyjson_mut_is_str(uriValue))
                throw std::runtime_error("ModelSource dependency URI is invalid");
            const auto uri = mutableString(uriValue).value_or(std::string{});
            emitted.insert(uri);
            const auto found = byUri.find(uri);
            if (found == byUri.end())
                continue;
            const auto id = dependencyIds.at(found->second.path);
            auto* dependencyObject = yyjson_mut_doc_ptr_get(document, prefix.c_str());
            if (yyjson_mut_doc_ptr_get(document, (prefix + "/assetId").c_str()))
                setJsonString(document, prefix + "/assetId", id);
            else
                yyjson_mut_obj_add_strcpy(document, dependencyObject, "assetId", id.c_str());
            if (yyjson_mut_doc_ptr_get(document, (prefix + "/pointer").c_str()))
                setJsonString(document, prefix + "/pointer", found->second.pointer);
            else
                yyjson_mut_obj_add_strcpy(document, dependencyObject, "pointer", found->second.pointer.c_str());
            if (yyjson_mut_doc_ptr_get(document, (prefix + "/sha256").c_str()))
                setJsonString(document, prefix + "/sha256", sha256(assetBytes(authoredPath(root, found->second.path))));
        }
        for (const auto& ref : refs) {
            if (emitted.contains(ref.uri))
                continue;
            const auto* id = &dependencyIds.at(ref.path);
            auto* object = yyjson_mut_obj(document);
            yyjson_mut_obj_add_strcpy(document, object, "uri", ref.uri.c_str());
            yyjson_mut_obj_add_strcpy(document, object, "sha256",
                                      sha256(assetBytes(authoredPath(root, ref.path))).c_str());
            yyjson_mut_obj_add_strcpy(document, object, "assetId", id->c_str());
            yyjson_mut_obj_add_strcpy(document, object, "pointer", ref.pointer.c_str());
            yyjson_mut_arr_append(dependencies, object);
        }
    });
}

FilePlan basePlan(const ProjectIndex& index, const std::string& label) {
    FilePlan plan;
    plan.label = label;
    for (const auto& file : index.files)
        addGuard(plan, index.root, file);
    for (const auto& directory : index.directories)
        addGuard(plan, index.root, directory);
    return plan;
}

FileEdit copiedEdit(const std::string& destination, const std::string& source, const std::optional<std::string>& text) {
    if (text)
        return {destination, FileKind::File, text, {}};
    return {destination, FileKind::File, std::nullopt, source};
}

void addSceneEditsForEnroll(const ProjectIndex& index, FilePlan& plan) {
    for (const auto& [path, scene] : index.scenes) {
        const auto meta = sidecarPath(path);
        if (index.metadata.contains(path))
            continue;
        addGuard(plan, index.root, path);
        addGuard(plan, index.root, meta);
        addEdit(plan, {meta, FileKind::File, newSceneMeta(scene.id), {}});
    }
}

FilePlan makeEnrollPlan(const ProjectIndex& index) {
    auto plan = basePlan(index, "Enroll project asset metadata");
    addSceneEditsForEnroll(index, plan);
    std::map<std::string, std::string> dependencyIds;

    for (const auto& [source, refs] : index.modelUris) {
        const auto metadataIt = index.metadata.find(source);
        if (metadataIt == index.metadata.end())
            throw std::runtime_error("ModelSource metadata missing: " + source);
        const auto& metadata = metadataIt->second;
        addGuard(plan, index.root, source);
        addGuard(plan, index.root, metadata.metaPath);
        for (const auto& ref : refs) {
            const auto dependencyMeta = index.metadata.find(ref.path);
            std::string id;
            if (dependencyMeta != index.metadata.end() && dependencyMeta->second.kind != "RawDependency")
                throw std::runtime_error("URI dependency sidecar has unsupported kind: " + dependencyMeta->second.kind);
            if (dependencyMeta != index.metadata.end() && dependencyMeta->second.kind == "RawDependency")
                id = dependencyMeta->second.id;
            if (id.empty() && dependencyIds.contains(ref.path))
                id = dependencyIds.at(ref.path);
            if (id.empty())
                id = AssetId::create().string();
            dependencyIds[ref.path] = id;
            addGuard(plan, index.root, ref.path);
            const auto metaPath = sidecarPath(ref.path);
            addGuard(plan, index.root, metaPath);
            if (dependencyMeta == index.metadata.end())
                addEdit(plan, {metaPath, FileKind::File, newRawDependencyMeta(id, metadata.id, ref.path), {}});
            else if (dependencyMeta->second.id.empty()) {
                const auto updated = sidecarWithIdentity(dependencyMeta->second.bytes, id, metadata.id, ref.path);
                if (updated != dependencyMeta->second.bytes)
                    addEdit(plan, {metaPath, FileKind::File, updated, {}});
            }
        }
        const auto updated = enrichModelMetadata(index.root, metadata, refs, dependencyIds);
        if (updated != metadata.bytes)
            addEdit(plan, {metadata.metaPath, FileKind::File, updated, {}});
    }
    return plan;
}

std::set<std::string> sceneSources(const SceneRecord& scene) {
    JsonDocument document(scene.bytes);
    std::set<std::string> result;
    if (auto* values = yyjson_obj_get(document.root(), "modelSources")) {
        if (!yyjson_is_arr(values))
            throw std::runtime_error("Scene modelSources is not an array");
        const auto count = yyjson_arr_size(values);
        for (size_t i = 0; i < count; ++i)
            result.insert(jsonText(yyjson_arr_get(values, i), "modelSources"));
    }
    return result;
}

std::set<std::string> sceneBehaviorAssetRefs(const SceneRecord& scene) {
    JsonDocument document(scene.bytes);
    std::set<std::string> result;
    auto* entities = yyjson_obj_get(document.root(), "entities");
    if (!entities || !yyjson_is_arr(entities))
        return result;
    const auto entityCount = yyjson_arr_size(entities);
    for (size_t i = 0; i < entityCount; ++i) {
        auto* entity = yyjson_arr_get(entities, i);
        auto* behaviors = entity ? yyjson_obj_get(entity, "behaviors") : nullptr;
        if (!behaviors || !yyjson_is_arr(behaviors))
            continue;
        const auto behaviorCount = yyjson_arr_size(behaviors);
        for (size_t b = 0; b < behaviorCount; ++b) {
            auto* binding = yyjson_arr_get(behaviors, b);
            auto* properties = binding ? yyjson_obj_get(binding, "properties") : nullptr;
            if (!properties || !yyjson_is_obj(properties))
                continue;
            yyjson_obj_iter propertyIterator = yyjson_obj_iter_with(properties);
            while (auto* propertyKey = yyjson_obj_iter_next(&propertyIterator)) {
                auto* propertyValue = yyjson_obj_iter_get_val(propertyKey);
                if (!propertyValue || !yyjson_is_obj(propertyValue))
                    continue;
                yyjson_obj_iter wrapperIterator = yyjson_obj_iter_with(propertyValue);
                auto* wrapperKey = yyjson_obj_iter_next(&wrapperIterator);
                if (!wrapperKey || yyjson_obj_iter_next(&wrapperIterator))
                    continue;
                const std::string_view wrapperName(yyjson_get_str(wrapperKey), yyjson_get_len(wrapperKey));
                if (wrapperName != "assetRef")
                    continue;
                auto* assetValue = yyjson_obj_iter_get_val(wrapperKey);
                if (!assetValue || yyjson_is_null(assetValue))
                    continue;
                if (!yyjson_is_str(assetValue))
                    throw std::runtime_error("Behavior AssetRef must contain a UUID or null");
                const auto id = AssetId::parse({yyjson_get_str(assetValue), yyjson_get_len(assetValue)});
                result.insert(lower(id.string()));
            }
        }
    }
    return result;
}

std::string normalizeDestination(const Path& root, const std::string& input) {
    if (input.empty() || input == ".")
        return {};
    if (isServicePath(input) || isMetaPath(input))
        throw std::runtime_error("Destination is a project control path");
    const auto path = projectPath(root, input, false);
    const auto relative = relString(rootPath(root), path);
    if (relative == ".")
        return {};
    const auto absolute = authoredPath(root, relative);
    if (std::filesystem::exists(absolute) && !std::filesystem::is_directory(absolute))
        throw std::runtime_error("Destination directory is not a directory");
    return relative;
}

std::string targetRootFor(const std::string& sourceRoot, const std::string& destination,
                          const std::optional<std::string>& newName) {
    return appendPath(destination, destinationName(sourceRoot, newName));
}

std::string targetForPath(const std::string& source, const std::string& sourceRoot, const std::string& targetRoot) {
    if (source == sourceRoot)
        return targetRoot;
    if (source == sourceRoot + ".meta")
        return targetRoot + ".meta";
    const auto suffix = utf8Path(source).lexically_relative(utf8Path(sourceRoot));
    if (suffix.empty() || suffix.is_absolute() || suffix.has_root_name() || *suffix.begin() == "..")
        throw std::runtime_error("Selected paths are not a single resource group");
    return appendPath(targetRoot, utf8(suffix.generic_wstring()));
}

void validateTargetExtension(const ProjectIndex& index, const std::string& source, const std::string& target) {
    if (isScenePath(source) && !isScenePath(target))
        throw std::runtime_error("Scene files must retain the complete .scene.json suffix");
    if ((isModelPath(source) || isScenePath(source)) && extension(source) != extension(target))
        throw std::runtime_error("Model and scene extensions cannot change during a file operation");
    if (isMetaPath(source) && !isMetaPath(target))
        throw std::runtime_error("Metadata sidecars must retain their source suffix");
    (void)index;
}

void buildIdMap(const ProjectIndex& index, FileOperation operation, const std::map<std::string, std::string>& mapped,
                std::map<std::string, std::string>& ids) {
    if (operation != FileOperation::Copy)
        return;
    for (const auto& [source, metadata] : index.metadata) {
        if (!mapped.contains(source))
            continue;
        if (!metadata.id.empty())
            ids.emplace(metadata.id, AssetId::create().string());
        for (const auto& [id, locator] : metadata.subAssets)
            ids.emplace(id, AssetId::create().string());
    }
    for (const auto& [source, scene] : index.scenes)
        if (mapped.contains(source))
            ids.emplace(scene.id, AssetId::create().string());
}

void checkRemoveReferences(const ProjectIndex& index, const SelectedTree& selected,
                           const std::map<std::string, std::string>& mapped) {
    for (const auto& [model, refs] : index.modelUris)
        for (const auto& ref : refs)
            if (selected.paths.contains(ref.path) && !selected.paths.contains(model)) {
                std::string sceneOwners;
                if (const auto metadata = index.metadata.find(model); metadata != index.metadata.end())
                    for (const auto& [scenePath, scene] : index.scenes)
                        if (sceneSources(scene).contains(metadata->second.id))
                            sceneOwners += (sceneOwners.empty() ? "" : ", ") + scene.id + "@" + scenePath;
                throw std::runtime_error("Cannot remove referenced URI " + ref.uri + " from " + model +
                                         (sceneOwners.empty() ? std::string{} : "; sceneSourceIDs: " + sceneOwners));
            }

    for (const auto& [scenePath, scene] : index.scenes) {
        if (selected.paths.contains(scenePath))
            continue;
        const auto sources = sceneSources(scene);
        for (const auto& [modelPath, metadata] : index.metadata)
            if (metadata.kind == "ModelSource" && selected.paths.contains(modelPath) && sources.contains(metadata.id))
                throw std::runtime_error("Cannot remove model source referenced by scene " + scenePath + ": " +
                                         metadata.id);
        for (const auto& assetId : sceneBehaviorAssetRefs(scene)) {
            const auto asset = index.idToPath.find(assetId);
            if (asset != index.idToPath.end() && selected.paths.contains(asset->second))
                throw std::runtime_error("Cannot remove AssetRef target referenced by scene " + scenePath + ": " +
                                         assetId);
        }
    }
    (void)mapped;
}

void checkTargetCollisions(const ProjectIndex& index, FileOperation operation, const SelectedTree& selected,
                           const std::map<std::string, std::string>& mapped) {
    std::set<std::string> keys;
    for (const auto& [source, target] : mapped) {
        if (operation == FileOperation::Move && source == target)
            throw std::runtime_error("Move destination is unchanged: " + source);
        if (!keys.insert(projectPathKey(target)).second)
            throw std::runtime_error("Two selected paths map to one destination: " + target);
        const bool caseFoldIdentity = projectPathKey(source) == projectPathKey(target);
        if (!caseFoldIdentity && sameOrDescendant(source, target))
            throw std::runtime_error("Destination is inside the selected source group");
        const auto stamp = projectStamp(index.root, target);
        if (stamp.kind != FileKind::Missing) {
            const bool caseOnlyMove =
                operation == FileOperation::Move && projectPathKey(source) == projectPathKey(target);
            if (!caseOnlyMove)
                throw std::runtime_error("Destination already exists: " + target);
        }
    }
    (void)selected;
}

std::map<std::string, std::string> selectedPathMap(const ProjectIndex& index, const SelectedTree& selected,
                                                   const std::string& destination,
                                                   const std::optional<std::string>& newName) {
    if (newName && selected.roots.size() != 1)
        throw std::runtime_error("A new name requires exactly one selected path");
    std::map<std::string, std::string> mapped;
    for (const auto& root : selected.roots) {
        const auto targetRoot = targetRootFor(root, destination, newName);
        for (const auto& path : selected.paths)
            if (sameOrDescendant(root, path) || path == root + ".meta") {
                const auto target = targetForPath(path, root, targetRoot);
                validateTargetExtension(index, path, target);
                mapped.emplace(path, target);
            }
        for (const auto& path : selected.directories)
            if (sameOrDescendant(root, path))
                mapped.emplace(path, targetForPath(path, root, targetRoot));
    }
    return mapped;
}

void addDestinationGuardsAndDirs(const ProjectIndex& index, FilePlan& plan, const std::string& target) {
    addGuard(plan, index.root, target);
    addDirectoryCreates(plan, index.root, target);
}

void addModelRewrite(const ProjectIndex& index, FilePlan& plan, FileOperation operation, const std::string& model,
                     const std::string& target, const std::vector<UriReference>& refs,
                     const std::map<std::string, std::string>& mapped, const std::map<std::string, std::string>& ids,
                     bool selectedModel) {
    std::map<std::string, std::string> uriUpdates;
    for (const auto& ref : refs) {
        const auto dependency = mapped.contains(ref.path) ? mapped.at(ref.path) : ref.path;
        const auto rewritten = relativeUri(index.root, target, dependency);
        if (rewritten != ref.uri)
            uriUpdates.emplace(ref.pointer, rewritten);
    }
    if (uriUpdates.empty())
        return;
    const auto bytes = readFile(index.root, model);
    const auto rewritten = rewriteModelBytes(bytes, extension(model) == ".glb", uriUpdates);
    if (selectedModel && operation != FileOperation::Remove)
        addEdit(plan, copiedEdit(target, model, rewritten));
    else
        addEdit(plan, {model, FileKind::File, rewritten, {}});

    const auto metadata = index.metadata.find(model);
    if (metadata != index.metadata.end()) {
        std::map<std::string, std::string> byUri;
        for (const auto& ref : refs)
            if (const auto found = uriUpdates.find(ref.pointer); found != uriUpdates.end())
                byUri.emplace(ref.uri, found->second);
        const auto metaBytes = rewriteMetadata(metadata->second, ids, byUri, std::nullopt);
        if (selectedModel)
            addEdit(plan, copiedEdit(mapped.at(metadata->second.metaPath), metadata->second.metaPath, metaBytes));
        else
            addEdit(plan, {metadata->second.metaPath, FileKind::File, metaBytes, {}});
    }
}

std::string mappedPathForMetadata(const std::string& metadataPath, const std::string& model, const std::string& target);

void addModelNoRewrite(const ProjectIndex& index, FilePlan& plan, const std::string& model, const std::string& target,
                       const std::map<std::string, std::string>& ids,
                       const std::map<std::string, std::string>& uriUpdates, bool selectedModel) {
    const auto metadata = index.metadata.find(model);
    if (metadata == index.metadata.end())
        throw std::runtime_error("ModelSource metadata missing: " + model);
    std::optional<std::string> metaBytes;
    const auto updatedMeta = rewriteMetadata(metadata->second, ids, uriUpdates, std::nullopt);
    if (updatedMeta != metadata->second.bytes)
        metaBytes = updatedMeta;
    if (selectedModel)
        addEdit(plan, copiedEdit(target, model, std::nullopt));
    if (selectedModel) {
        const auto metaTarget = mappedPathForMetadata(metadata->second.metaPath, model, target);
        addEdit(plan, copiedEdit(metaTarget, metadata->second.metaPath, metaBytes));
    } else if (metaBytes)
        addEdit(plan, {metadata->second.metaPath, FileKind::File, metaBytes, {}});
}

std::string mappedPathForMetadata(const std::string& metadataPath, const std::string& model,
                                  const std::string& target) {
    if (metadataPath == model + ".meta")
        return target + ".meta";
    const auto modelParent = utf8Path(model).parent_path();
    const auto targetParent = utf8Path(target).parent_path();
    const auto suffix = utf8Path(metadataPath).lexically_relative(modelParent);
    return appendPath(utf8(targetParent.generic_wstring()), utf8(suffix.generic_wstring()));
}

FilePlan makeFilePlan(const ProjectIndex& index, FileOperation operation, const std::vector<std::string>& sourcePaths,
                      const std::string& destinationDirectory, const std::optional<std::string>& newName) {
    ensureName(newName);
    if (operation == FileOperation::Remove) {
        if (!destinationDirectory.empty() || newName)
            throw std::runtime_error("Remove does not accept a destination");
    }
    const auto destination =
        operation == FileOperation::Remove ? std::string{} : normalizeDestination(index.root, destinationDirectory);
    const auto selected = selectPaths(index, sourcePaths);
    const auto mapped = operation == FileOperation::Remove ? std::map<std::string, std::string>{}
                                                           : selectedPathMap(index, selected, destination, newName);

    if (operation != FileOperation::Remove) {
        for (const auto& [source, target] : mapped) {
            const auto metadata = index.metadata.find(source);
            const bool sourceAsset =
                isModelPath(source) || (metadata != index.metadata.end() && (metadata->second.kind == "ModelSource" ||
                                                                             metadata->second.kind == "RawDependency"));
            if (sourceAsset && !(target == "Assets" || target.starts_with("Assets/")))
                throw std::runtime_error("Model sources and URI dependencies must remain inside Assets");
        }
    }

    if (operation != FileOperation::Remove)
        checkTargetCollisions(index, operation, selected, mapped);
    else
        checkRemoveReferences(index, selected, mapped);

    std::map<std::string, std::string> ids;
    buildIdMap(index, operation, mapped, ids);

    FilePlan plan;
    plan.label = operation == FileOperation::Move   ? "Move project files"
                 : operation == FileOperation::Copy ? "Copy project files"
                                                    : "Remove project files";

    // Guard every selected authored path and every parsed model/scene dependency
    // before the transaction is allowed to mutate anything.
    for (const auto& path : selected.paths)
        addGuard(plan, index.root, path);
    for (const auto& path : selected.directories)
        addGuard(plan, index.root, path);
    for (const auto& directory : index.directories)
        addGuard(plan, index.root, directory);
    for (const auto& [source, metadata] : index.metadata) {
        addGuard(plan, index.root, source);
        addGuard(plan, index.root, metadata.metaPath);
    }
    for (const auto& [scenePath, scene] : index.scenes) {
        addGuard(plan, index.root, scenePath);
        if (std::filesystem::exists(authoredPath(index.root, scene.metaPath)))
            addGuard(plan, index.root, scene.metaPath);
    }
    for (const auto& [model, refs] : index.modelUris) {
        addGuard(plan, index.root, model);
        for (const auto& ref : refs)
            addGuard(plan, index.root, ref.path);
    }

    if (operation == FileOperation::Remove) {
        for (const auto& path : selected.paths)
            addMissingEdit(plan, path);
        for (const auto& path : selected.directories)
            addMissingEdit(plan, path);
        return plan;
    }

    std::set<std::string> handledModels;
    std::set<std::string> handledScenes;
    std::set<std::string> handledMetadata;

    // A moved dependency requires URI rewrites in every model that still
    // points at it. Copies deliberately leave external references untouched.
    for (const auto& [model, refs] : index.modelUris) {
        const bool selectedModel = mapped.contains(model);
        bool dependencyMoved = false;
        if (operation == FileOperation::Move)
            for (const auto& ref : refs)
                if (mapped.contains(ref.path) && mapped.at(ref.path) != ref.path)
                    dependencyMoved = true;
        if (!selectedModel && !dependencyMoved)
            continue;
        addGuard(plan, index.root, model);
        if (const auto metadata = index.metadata.find(model); metadata != index.metadata.end())
            addGuard(plan, index.root, metadata->second.metaPath);
        const auto target = selectedModel ? mapped.at(model) : model;
        std::map<std::string, std::string> uriUpdates;
        for (const auto& ref : refs) {
            const auto dependency = mapped.contains(ref.path) ? mapped.at(ref.path) : ref.path;
            const auto rewritten = relativeUri(index.root, target, dependency);
            if (rewritten != ref.uri)
                uriUpdates.emplace(ref.pointer, rewritten);
        }
        if (uriUpdates.empty())
            addModelNoRewrite(index, plan, model, target, ids, {}, selectedModel);
        else
            addModelRewrite(index, plan, operation, model, target, refs, mapped, ids, selectedModel);
        handledModels.insert(model);
        handledMetadata.insert(index.metadata.at(model).metaPath);
    }

    // Scene copy/move preserves EntityIds, updates assetRoot, and remaps only
    // typed references whose resources were copied in this same operation.
    for (const auto& [source, scene] : index.scenes) {
        if (!mapped.contains(source))
            continue;
        const auto target = mapped.at(source);
        const auto newId =
            operation == FileOperation::Copy ? std::optional<std::string>(ids.at(scene.id)) : std::nullopt;
        const auto updated = rewriteScene(scene, ids, sceneAssetRoot(index.root, target), newId);
        addEdit(plan, copiedEdit(target, source,
                                 updated == scene.bytes ? std::nullopt : std::optional<std::string>(updated)));
        handledScenes.insert(source);
        if (const auto metadata = index.metadata.find(source); metadata != index.metadata.end()) {
            const auto metaTarget =
                mapped.contains(metadata->second.metaPath) ? mapped.at(metadata->second.metaPath) : target + ".meta";
            const auto meta = rewriteMetadata(metadata->second, ids, {}, std::optional<std::string>(target));
            addEdit(plan, copiedEdit(metaTarget, metadata->second.metaPath,
                                     meta == metadata->second.bytes ? std::nullopt : std::optional<std::string>(meta)));
            handledMetadata.insert(metadata->second.metaPath);
        }
    }

    // The remaining selected files are ordinary authored bytes or metadata
    // sidecars.  Metadata follows its source and receives the UUID map.
    for (const auto& path : selected.paths) {
        if (selected.directories.contains(path))
            continue;
        if (handledModels.contains(path) || handledScenes.contains(path))
            continue;
        if (isMetaPath(path) && handledMetadata.contains(path))
            continue;
        const auto target = mapped.at(path);
        if (isMetaPath(path)) {
            const auto source = removeMetaSuffix(path);
            if (const auto metadata = index.metadata.find(source); metadata != index.metadata.end()) {
                const auto sourceTarget =
                    mapped.contains(source) ? std::optional<std::string>(mapped.at(source)) : std::nullopt;
                const auto updated = rewriteMetadata(metadata->second, ids, {}, sourceTarget);
                addEdit(plan, copiedEdit(target, path,
                                         updated == metadata->second.bytes ? std::nullopt
                                                                           : std::optional<std::string>(updated)));
            } else
                addEdit(plan, copiedEdit(target, path, std::nullopt));
        } else
            addEdit(plan, copiedEdit(target, path, std::nullopt));
    }
    for (const auto& directory : selected.directories) {
        const auto target = mapped.at(directory);
        addEdit(plan, {target, FileKind::Directory, std::nullopt, {}});
    }

    if (operation == FileOperation::Move) {
        for (const auto& path : selected.paths)
            addMissingEdit(plan, path);
        for (const auto& directory : selected.directories)
            addMissingEdit(plan, directory);
        std::vector<std::pair<std::string, std::string>> pathMoves(mapped.begin(), mapped.end());
        std::stable_sort(pathMoves.begin(), pathMoves.end(), [](const auto& left, const auto& right) {
            const auto depth = [](const std::string& path) { return std::count(path.begin(), path.end(), '/'); };
            const auto leftDepth = depth(left.first), rightDepth = depth(right.first);
            return leftDepth == rightDepth ? left.first < right.first : leftDepth < rightDepth;
        });
        for (const auto& [before, after] : pathMoves)
            plan.moves.push_back({before, after});
    }

    std::vector<std::string> destinationPaths;
    for (const auto& edit : plan.edits)
        if (edit.kind == FileKind::File || edit.kind == FileKind::Directory)
            destinationPaths.push_back(edit.path);
    for (const auto& path : destinationPaths)
        addDestinationGuardsAndDirs(index, plan, path);
    return plan;
}

} // namespace

std::vector<ProjectAsset> inspectProjectAssets(const std::filesystem::path& root) {
    return assetsFromIndex(indexProject(root));
}

FilePlan enrollProjectFiles(const std::filesystem::path& root) {
    return makeEnrollPlan(indexProject(root));
}

FilePlan planProjectFiles(const std::filesystem::path& root, FileOperation operation,
                          const std::vector<std::string>& sourcePaths, const std::string& destinationDirectory,
                          const std::optional<std::string>& newName) {
    return makeFilePlan(indexProject(root), operation, sourcePaths, destinationDirectory, newName);
}

} // namespace proto
