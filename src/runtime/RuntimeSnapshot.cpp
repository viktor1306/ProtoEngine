#include "runtime/RuntimeSnapshot.hpp"
#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "scene/SceneIO.hpp"
#include "core/Diagnostics.hpp"
#include <windows.h>
#include <algorithm>
#include <map>
#include <set>

namespace proto {
namespace fs = std::filesystem;
namespace {
void ordinaryPath(const fs::path& path) {
    const auto full = fs::absolute(path).lexically_normal();
    fs::path current = full.root_path();
    for (const auto& part : full.relative_path()) {
        current /= part;
        const auto attr = GetFileAttributesW(current.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Play data path contains a reparse point");
    }
}
fs::path inside(const fs::path& boundary, const fs::path& base, const std::string& relative) {
    auto path = utf8Path(relative);
    if (path.is_absolute() || path.has_root_path() || relative.find(':') != std::string::npos ||
        relative.find('\\') != std::string::npos)
        throw std::runtime_error("Invalid runtime relative path");
    const auto root = fs::absolute(boundary).lexically_normal();
    const auto resolved = fs::absolute(base / path).lexically_normal();
    const auto suffix = resolved.lexically_relative(root);
    if (suffix.empty() || suffix == "." || *suffix.begin() == "..")
        throw std::runtime_error("Runtime path escapes snapshot boundary");
    ordinaryPath(resolved);
    return resolved;
}
std::string textFile(const fs::path& path, size_t limit = 32 * 1024 * 1024) {
    const auto data = assetBytes(path, limit);
    return {data.begin(), data.end()};
}
void keys(Json* object, std::initializer_list<const char*> allowed) {
    if (!yyjson_is_obj(object))
        throw std::runtime_error("Runtime manifest object expected");
    std::set<std::string> seen;
    size_t i{}, n{};
    Json *key{}, *value{};
    yyjson_obj_foreach(object, i, n, key, value) {
        const std::string name = yyjson_get_str(key);
        if (!seen.insert(name).second ||
            std::none_of(allowed.begin(), allowed.end(), [&](const char* x) { return name == x; }))
            throw std::runtime_error("Unknown or duplicate runtime manifest field: " + name);
    }
}
std::string fileHash(const fs::path& path) {
    return sha256(assetBytes(path, 512 * 1024 * 1024));
}
} // namespace
SnapshotPins::~SnapshotPins() {
    for (auto handle : handles_)
        CloseHandle(handle);
}
void SnapshotPins::add(const fs::path& file) {
    ordinaryPath(file);
    auto handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot pin Play data file: " + utf8(file.wstring()));
    handles_.push_back(handle);
}
void SnapshotPins::addDirectory(const fs::path& directory) {
    ordinaryPath(directory);
    const auto handle = CreateFileW(directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot pin runtime package directory");
    handles_.push_back(handle);
}
PlaySnapshot stagePlaySnapshot(const fs::path& projectRoot, const Scene& scene, std::string_view sdkBuildId) {
    const auto root = fs::absolute(projectRoot).lexically_normal();
    const auto cache = root / ".proto" / "play-cache";
    const auto run = root / ".proto" / "snapshots" / Uuid::create().string();
    ordinaryPath(cache);
    ordinaryPath(run);
    fs::create_directories(cache);
    fs::create_directories(run);
    PlaySnapshot result{run / "runtime.json", std::make_shared<SnapshotPins>()};
    std::string entries;
    std::vector<AssetId> models;
    for (const auto& [id, bundle] : scene.assets->models) {
        (void)bundle;
        models.push_back(id);
    }
    std::sort(models.begin(), models.end());
    // Include current catalog material revisions, including unsaved in-memory overrides.
    for (const auto id : models) {
        ModelBundle bundle = *scene.assets->models.at(id);
        for (auto& material : bundle.materials) {
            const auto it = scene.assets->materials.find(material->id);
            if (it != scene.assets->materials.end())
                material = it->second;
        }
        const auto temporary = cache / (Uuid::create().string() + ".tmp");
        try {
            saveCooked(temporary, bundle);
            const auto hash = fileHash(temporary);
            const auto blob = cache / (hash + ".bin");
            ordinaryPath(blob);
            if (!fs::exists(blob)) {
                if (!MoveFileExW(temporary.c_str(), blob.c_str(), MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("Cannot publish cooked Play blob");
            } else
                fs::remove(temporary);
            result.pins->add(blob);
            if (fileHash(blob) != hash)
                throw std::runtime_error("Existing Play blob hash mismatch");
            const auto size = fs::file_size(blob);
            if (!entries.empty())
                entries += ',';
            entries += "{\"id\":" + jsonString(id.string()) +
                       ",\"path\":" + jsonString("../../play-cache/" + hash + ".bin") +
                       ",\"sha256\":" + jsonString(hash) + ",\"bytes\":" + std::to_string(size) + "}";
            ++result.blobs;
            result.bytes += size;
        } catch (...) {
            std::error_code ec;
            fs::remove(temporary, ec);
            throw;
        }
    }
    std::string rawEntries;
    std::vector<AssetId> rawIds;
    size_t rawBytes{};
    for (const auto& [id, data] : scene.assets->dataFiles) {
        if (!data || data->size() > 64 * 1024 * 1024)
            throw std::runtime_error("Raw Play asset exceeds 64 MiB or has no data");
        rawBytes += data->size();
        if (rawBytes > size_t(2) * 1024 * 1024 * 1024 || rawIds.size() >= 16384)
            throw std::runtime_error("Raw Play assets exceed the session limit");
        rawIds.push_back(id);
    }
    std::sort(rawIds.begin(), rawIds.end());
    for (const auto id : rawIds) {
        const auto& data = *scene.assets->dataFiles.at(id);
        const auto hash = sha256(data);
        const auto blob = cache / (hash + ".data");
        ordinaryPath(blob);
        if (!fs::exists(blob))
            atomicWrite(blob, std::string(data.begin(), data.end()));
        result.pins->add(blob);
        if (fileHash(blob) != hash)
            throw std::runtime_error("Existing raw Play blob hash mismatch");
        if (!rawEntries.empty())
            rawEntries += ',';
        rawEntries += "{\"id\":" + jsonString(id.string()) + ",\"path\":" +
                      jsonString("../../play-cache/" + hash + ".data") + ",\"sha256\":" + jsonString(hash) +
                      ",\"bytes\":" + std::to_string(data.size()) + "}";
        ++result.blobs;
        result.bytes += data.size();
    }
    auto snapshot = scene.snapshot();
    snapshot.assetRoot.clear();
    const auto sceneBytes = encodeScene(Scene::fromSnapshot(snapshot, scene.assets));
    result.sceneHash = sha256(sceneBytes);
    atomicWrite(run / "scene.scene.json", sceneBytes);
    result.pins->add(run / "scene.scene.json");
    atomicWrite(result.manifest,
                "{\"format\":\"proto.play\",\"formatVersion\":1,\"sdkBuildId\":" + jsonString(sdkBuildId) +
                    ",\"scene\":\"scene.scene.json\",\"sceneHash\":" + jsonString(result.sceneHash) + ",\"models\":[" +
                    entries + "],\"rawAssets\":[" + rawEntries + "]}");
    result.pins->add(result.manifest);
    return result;
}
Scene loadRuntimeSnapshot(const fs::path& input, std::string_view expectedBuildId) {
    const auto manifest = fs::absolute(input).lexically_normal();
    ordinaryPath(manifest);
    SnapshotPins pins;
    pins.add(manifest);
    const auto base = manifest.parent_path();
    // M5's private snapshots live in .proto/snapshots/<run>. No authored path is consumed.
    if (base.parent_path().filename() != "snapshots" || base.parent_path().parent_path().filename() != ".proto")
        throw std::runtime_error("Expected an M5 .proto/snapshots runtime manifest");
    const auto boundary = base.parent_path().parent_path();
    JsonDoc doc(textFile(manifest, 4 * 1024 * 1024));
    auto* object = doc.root();
    keys(object, {"format", "formatVersion", "sdkBuildId", "scene", "sceneHash", "models", "rawAssets"});
    if (str(get(object, "format")) != "proto.play" || num(get(object, "formatVersion")) != 1 ||
        str(get(object, "sdkBuildId")) != expectedBuildId)
        throw std::runtime_error("Incompatible Play snapshot/SDK");
    const auto scenePath = inside(base, base, str(get(object, "scene")));
    pins.add(scenePath);
    const auto sceneBytes = textFile(scenePath);
    if (sha256(sceneBytes) != str(get(object, "sceneHash")))
        throw std::runtime_error("Play scene hash mismatch");
    auto catalog = std::make_shared<AssetCatalog>();
    auto* models = get(object, "models");
    if (!yyjson_is_arr(models) || yyjson_arr_size(models) > 16384)
        throw std::runtime_error("Invalid Play model list");
    size_t i{}, n{};
    Json* entry{};
    std::set<AssetId> seen;
    yyjson_arr_foreach(models, i, n, entry) {
        keys(entry, {"id", "path", "sha256", "bytes"});
        const auto id = AssetId::parse(str(get(entry, "id")));
        if (!seen.insert(id).second)
            throw std::runtime_error("Duplicate Play model ID");
        const auto hash = str(get(entry, "sha256"));
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid Play model hash");
        const auto path = inside(boundary, base, str(get(entry, "path")));
        if (path.parent_path() != boundary / "play-cache" || path.filename() != hash + ".bin")
            throw std::runtime_error("Play blob is not content-addressed");
        pins.add(path);
        auto* length = get(entry, "bytes");
        if (!yyjson_is_uint(length) || fs::file_size(path) != yyjson_get_uint(length) || fileHash(path) != hash)
            throw std::runtime_error("Play blob integrity mismatch");
        auto bundle = readCooked(path);
        if (bundle->id != id)
            throw std::runtime_error("Play blob asset ID mismatch");
        catalog->publish(std::move(bundle));
    }
    if (auto* raw = yyjson_obj_get(object, "rawAssets")) {
        if (!yyjson_is_arr(raw) || yyjson_arr_size(raw) > 16384)
            throw std::runtime_error("Invalid raw Play asset list");
        size_t totalRawBytes{};
        std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> payloads;
        yyjson_arr_foreach(raw, i, n, entry) {
            keys(entry, {"id", "path", "sha256", "bytes"});
            const auto id = AssetId::parse(str(get(entry, "id")));
            if (!seen.insert(id).second || catalog->meshes.contains(id) || catalog->materials.contains(id) ||
                catalog->textures.contains(id) || id == builtin::cube || id == builtin::plane)
                throw std::runtime_error("Duplicate raw Play asset ID");
            const auto hash = str(get(entry, "sha256"));
            if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Invalid raw Play asset hash");
            const auto path = inside(boundary, base, str(get(entry, "path")));
            if (path.parent_path() != boundary / "play-cache" || path.filename() != hash + ".data")
                throw std::runtime_error("Raw Play blob is not content-addressed");
            auto* length = get(entry, "bytes");
            if (!yyjson_is_uint(length) || yyjson_get_uint(length) > 64 * 1024 * 1024)
                throw std::runtime_error("Invalid raw Play asset size");
            totalRawBytes += static_cast<size_t>(yyjson_get_uint(length));
            if (totalRawBytes > size_t(2) * 1024 * 1024 * 1024)
                throw std::runtime_error("Raw Play assets exceed the session limit");
            auto [cached, fresh] = payloads.try_emplace(hash);
            if (fresh) {
                pins.add(path);
                auto data = std::make_shared<std::vector<uint8_t>>(assetBytes(path, 64 * 1024 * 1024));
                if (data->size() != yyjson_get_uint(length) || sha256(*data) != hash)
                    throw std::runtime_error("Raw Play blob integrity mismatch");
                cached->second = std::move(data);
            } else if (cached->second->size() != yyjson_get_uint(length)) {
                throw std::runtime_error("Raw Play blob size disagreement");
            }
            catalog->dataFiles.emplace(id, cached->second);
        }
    }
    auto scene = decodeScene(sceneBytes, std::move(catalog));
    if (!scene.assetRoot.empty())
        throw std::runtime_error("Runtime cannot read authored asset roots");
    return scene;
}
} // namespace proto
