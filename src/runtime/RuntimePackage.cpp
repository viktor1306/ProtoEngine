#include "runtime/RuntimePackage.hpp"
#include "runtime/PlayerGraphics.hpp"
#include "assets/AssetIO.hpp"
#include "assets/PortableAssets.hpp"
#include "core/Diagnostics.hpp"
#include "scene/SceneIO.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace proto {
namespace fs = std::filesystem;
namespace {
constexpr size_t maxFileBytes = 512 * 1024 * 1024;
constexpr size_t maxRawBytes = 64 * 1024 * 1024;
constexpr size_t maxEntries = 65536;
constexpr uint64_t maxPackageBytes = uint64_t(8) * 1024 * 1024 * 1024;

struct FileRecord {
    std::string path, hash;
    uint64_t bytes{};
};
struct AssetRecord {
    AssetId id;
    std::string type, path;
    std::vector<AssetId> dependencies;
    bool operator==(const AssetRecord&) const = default;
};
using Files = std::map<std::string, FileRecord>;
using Records = std::map<AssetId, AssetRecord>;
bool mutableFile(const std::string& path) {
    if (path == "Data/runtime.json" || path == "Config/graphics.user.json" ||
        path == "Config/graphics.user.json.bak" || path == "Player.log")
        return true;
    if (path.starts_with("Config/.tmp-")) {
        auto id = path.substr(12);
        if (id.ends_with("-backup"))
            id.resize(id.size() - 7);
        try {
            (void)Uuid::parse(id);
            return true;
        } catch (...) {
        }
    }
    return false;
}

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("Runtime package: " + message);
}
std::wstring native(const fs::path& path) {
    const auto absolute = fs::absolute(path).lexically_normal().make_preferred().native();
    if (absolute.starts_with(L"\\\\?\\"))
        return absolute;
    if (absolute.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    return L"\\\\?\\" + absolute;
}
void ordinary(const fs::path& path, bool requireFile = false) {
    const auto full = fs::absolute(path).lexically_normal();
    auto current = full.root_path();
    for (const auto& component : full.relative_path()) {
        current /= component;
        const auto attributes = GetFileAttributesW(native(current).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                invalid("cannot inspect a data path");
            continue;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            invalid("data path contains a reparse point");
    }
    if (requireFile) {
        const auto attributes = GetFileAttributesW(native(full).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            invalid("missing regular file: " + utf8(full.filename().wstring()));
    }
}
std::wstring fold(const fs::path& path) {
    const auto text = path.generic_wstring();
    const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count)
        invalid("cannot normalize a file name");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()),
                       result.data(), count, nullptr, nullptr, 0))
        invalid("cannot normalize a file name");
    return result;
}
fs::path relative(const std::string& text) {
    if (text.empty() || text.size() > 4096 || !validUtf8(text) || text.find('\0') != std::string::npos ||
        text.find_first_of("\\:<>\"|*?") != std::string::npos || text.find("//") != std::string::npos)
        invalid("invalid relative file path");
    const auto path = utf8Path(text);
    if (path.is_absolute() || path.has_root_path() || path.generic_wstring() != path.lexically_normal().generic_wstring())
        invalid("non-canonical relative file path");
    for (const auto& part : path) {
        const auto name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
            std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
            invalid("unsafe relative file name");
        auto stem = fold(part);
        stem = stem.substr(0, stem.find(L'.'));
        if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
            (stem.size() == 4 && (stem.starts_with(L"com") || stem.starts_with(L"lpt")) &&
             stem.back() >= L'1' && stem.back() <= L'9'))
            invalid("reserved Windows file name");
    }
    return path;
}
fs::path resolve(const fs::path& root, const std::string& text, bool required = true) {
    const auto result = root / relative(text);
    ordinary(result, required);
    return result;
}
uint64_t fileSize(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        invalid("cannot read immutable file size");
    return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}
std::vector<uint8_t> readBytes(const fs::path& path, size_t limit = maxFileBytes) {
    ordinary(path, true);
    return assetBytes(fs::path(native(path)), limit);
}
std::string readText(const fs::path& path, size_t limit) {
    const auto bytes = readBytes(path, limit);
    return {bytes.begin(), bytes.end()};
}
void writeBytes(const fs::path& path, std::span<const uint8_t> bytes) {
    ordinary(path);
    fs::create_directories(fs::path(native(path.parent_path())));
    ordinary(path.parent_path());
    if (GetFileAttributesW(native(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (readBytes(path) != std::vector<uint8_t>(bytes.begin(), bytes.end()))
            invalid("staged content-addressed file differs");
        return;
    }
    std::ofstream output(fs::path(native(path)), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output)
        invalid("cannot write staged resource");
}
void writeText(const fs::path& path, const std::string& text) {
    writeBytes(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
void keys(Json* value, std::initializer_list<const char*> allowed) {
    if (!yyjson_is_obj(value))
        invalid("object expected");
    std::set<std::string> seen;
    size_t i{}, n{};
    Json *key{}, *item{};
    yyjson_obj_foreach(value, i, n, key, item) {
        const auto name = str(key);
        if (!seen.insert(name).second ||
            std::none_of(allowed.begin(), allowed.end(), [&](const char* candidate) { return name == candidate; }))
            invalid("unknown or duplicate field: " + name);
    }
}
uint64_t integer(Json* value, uint64_t limit) {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > limit)
        invalid("invalid integer or size");
    return yyjson_get_uint(value);
}
void array(Json* value) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > maxEntries)
        invalid("invalid or oversized array");
}
void hashText(const std::string& text) {
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
        invalid("invalid content hash");
}
void textField(const std::string& text, size_t limit) {
    if (text.empty() || text.size() > limit || !validUtf8(text) || text.find('\0') != std::string::npos)
        invalid("invalid text field");
}
void uniqueIds(std::vector<AssetId>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
void insert(Records& records, AssetRecord record) {
    if (!record.id || record.id == builtin::cube || record.id == builtin::plane)
        invalid("nil or reserved resource ID");
    uniqueIds(record.dependencies);
    if (!records.emplace(record.id, std::move(record)).second || records.size() > maxEntries)
        invalid("duplicate or excessive resource IDs");
}
Records resourceRecords(const Scene& scene, const std::map<AssetId, std::string>& models,
                        const std::map<AssetId, std::string>& data, const std::string& scenePath) {
    Records result;
    std::vector<AssetId> sceneDependencies;
    for (const auto& [id, path] : models) {
        const auto& model = *scene.assets->models.at(id);
        std::vector<AssetId> dependencies;
        for (const auto& mesh : model.meshes) {
            dependencies.push_back(mesh->id);
            std::vector<AssetId> materials;
            for (const auto& part : mesh->parts)
                if (part.material)
                    materials.push_back(part.material);
            insert(result, {mesh->id, "Mesh", path, std::move(materials)});
        }
        for (const auto& bundled : model.materials) {
            const auto material = scene.assets->materials.at(bundled->id);
            dependencies.push_back(material->id);
            std::vector<AssetId> textures;
            for (const auto& slot : material->textures)
                if (slot.texture)
                    textures.push_back(slot.texture);
            insert(result, {material->id, "Material", path, std::move(textures)});
        }
        for (const auto& texture : model.textures) {
            dependencies.push_back(texture->id);
            insert(result, {texture->id, "Texture", path, {}});
        }
        insert(result, {id, "Model", path, std::move(dependencies)});
        sceneDependencies.push_back(id);
    }
    for (const auto& [id, path] : data) {
        insert(result, {id, "RawData", path, {}});
        sceneDependencies.push_back(id);
    }
    const auto snapshot = scene.snapshot();
    for (const auto id : snapshot.modelSources) {
        if (!models.contains(id))
            invalid("scene model source is missing");
        sceneDependencies.push_back(id);
    }
    for (const auto& entity : snapshot.entities) {
        if (!entity.mesh)
            continue;
        const auto& mesh = *entity.mesh;
        if (mesh.mesh != builtin::cube && mesh.mesh != builtin::plane) {
            if (!scene.assets->meshes.contains(mesh.mesh))
                invalid("scene mesh resource is missing");
            sceneDependencies.push_back(mesh.mesh);
        }
        for (const auto id : mesh.materials) {
            if (!id)
                continue;
            if (!scene.assets->materials.contains(id))
                invalid("scene material resource is missing");
            sceneDependencies.push_back(id);
        }
    }
    insert(result, {snapshot.id, "Scene", scenePath, std::move(sceneDependencies)});
    for (const auto& [id, record] : result) {
        (void)id;
        for (const auto dependency : record.dependencies)
            if (!result.contains(dependency))
                invalid("resource dependency is missing: " + dependency.string());
    }
    return result;
}
std::vector<std::string> listFiles(const fs::path& root) {
    struct FindHandle {
        HANDLE value;
        ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
    };
    std::vector<fs::path> pending{root};
    std::vector<std::string> result;
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        ordinary(directory);
        WIN32_FIND_DATAW data{};
        FindHandle find{FindFirstFileW((native(directory) + L"\\*").c_str(), &data)};
        if (find.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                continue;
            invalid("cannot enumerate staged directory");
        }
        do {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..")
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                invalid("staged package contains a reparse point");
            const auto path = directory / data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                pending.push_back(path);
            else {
                auto local = utf8(path.lexically_relative(root).generic_wstring());
                (void)relative(local);
                if (!mutableFile(local))
                    result.push_back(std::move(local));
            }
            if (result.size() + pending.size() > maxEntries)
                invalid("too many package files");
        } while (FindNextFileW(find.value, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            invalid("cannot finish staged directory enumeration");
    }
    std::sort(result.begin(), result.end());
    return result;
}
bool immutablePathAllowed(const std::string& path, const std::string& executable) {
    if (path == executable || path == "Config/graphics.json" || path == "THIRD_PARTY_NOTICES.txt" ||
        path == "THIRD_PARTY_NOTICES.md" || path == "Data/dll-audit.json")
        return true;
    const auto relativePath = relative(path);
    if (!relativePath.has_parent_path() && fold(relativePath.extension()) == L".dll")
        return true;
    return path.starts_with("Licenses/") || path.starts_with("Data/blobs/") ||
           path.starts_with("Data/scenes/") || path.starts_with("Data/shaders/");
}
void requiredFiles(const Files& files, const std::string& executable, const std::string& scenePath) {
    for (const auto& name : {executable, scenePath, std::string("Config/graphics.json")})
        if (!files.contains(name))
            invalid("required file is missing: " + name);
    if (!files.contains("THIRD_PARTY_NOTICES.txt") && !files.contains("THIRD_PARTY_NOTICES.md"))
        invalid("third-party notices are missing");
    bool hasLicense{};
    for (const auto& [name, file] : files) {
        (void)file;
        hasLicense = hasLicense || name.starts_with("Licenses/");
        if (!immutablePathAllowed(name, executable))
            invalid("unexpected immutable file: " + name);
    }
    if (!hasLicense)
        invalid("license files are missing");
    constexpr const char* shaders[] = {"triangle.vert", "triangle.frag", "scene.vert", "scene.frag",
        "pbr.vert", "pbr.frag", "lit.vert", "lit.frag", "shadow.vert", "shadow.frag",
        "camera_depth.vert", "camera_depth.frag", "light_tiles.comp", "tone.vert", "tone.frag",
        "player_present.vert", "player_present.frag"};
    for (const auto* shader : shaders)
        if (!files.contains("Data/shaders/" + std::string(shader) + ".spv"))
            invalid("shader is missing: " + std::string(shader));
}
std::string idsJson(std::span<const AssetId> ids) {
    std::string result{"["};
    for (const auto id : ids) {
        if (result.size() > 1) result += ',';
        result += jsonString(id.string());
    }
    return result + "]";
}
std::string assetsJson(const std::map<AssetId, std::string>& assets) {
    std::string result{"["};
    for (const auto& [id, path] : assets) {
        if (result.size() > 1) result += ',';
        result += "{\"id\":" + jsonString(id.string()) + ",\"path\":" + jsonString(path) + "}";
    }
    return result + "]";
}
std::map<AssetId, std::string> assetList(Json* value) {
    array(value);
    std::map<AssetId, std::string> result;
    size_t i{}, n{};
    Json* item{};
    yyjson_arr_foreach(value, i, n, item) {
        keys(item, {"id", "path"});
        const auto id = AssetId::parse(str(get(item, "id")));
        const auto path = str(get(item, "path"));
        (void)relative(path);
        if (!result.emplace(id, path).second)
            invalid("duplicate model/data ID");
    }
    return result;
}
std::vector<AssetId> idList(Json* value) {
    array(value);
    std::vector<AssetId> result;
    size_t i{}, n{};
    Json* item{};
    yyjson_arr_foreach(value, i, n, item) result.push_back(AssetId::parse(str(item)));
    const auto count = result.size();
    uniqueIds(result);
    if (count != result.size())
        invalid("duplicate dependency/additional asset ID");
    return result;
}
} // namespace

RuntimePackageStats writeRuntimePackage(const fs::path& input, const Scene& scene,
                                       const RuntimePackageMetadata& metadata,
                                       std::span<const PackageRawAsset> rawAssets) {
    const auto root = fs::absolute(input).lexically_normal();
    ordinary(root);
    if (!metadata.projectId || !scene.assets)
        invalid("project/catalog is missing");
    textField(metadata.name, 512);
    textField(metadata.sdkBuildId, 128);
    const auto executable = relative(metadata.executableName);
    if (executable.has_parent_path() || fold(executable.extension()) != L".exe")
        invalid("executable name must be a filename ending in .exe");
    if (GetFileAttributesW(native(root / "Data/runtime.json").c_str()) != INVALID_FILE_ATTRIBUTES)
        invalid("manifest already exists in staging");
    std::map<AssetId, std::string> modelPaths, dataPaths;
    std::vector<AssetId> modelIds;
    for (const auto& [id, model] : scene.assets->models) {
        (void)model;
        modelIds.push_back(id);
    }
    std::sort(modelIds.begin(), modelIds.end());
    for (const auto id : modelIds) {
        auto model = *scene.assets->models.at(id);
        for (auto& material : model.materials)
            if (const auto it = scene.assets->materials.find(material->id); it != scene.assets->materials.end())
                material = it->second;
        const auto bytes = encodePortableModel(model);
        const auto path = "Data/blobs/" + sha256(bytes) + ".model";
        writeBytes(root / utf8Path(path), bytes);
        modelPaths.emplace(id, path);
    }
    auto dataFiles = scene.assets->dataFiles;
    for (const auto& raw : rawAssets) {
        if (!raw.id)
            invalid("raw asset has no ID");
        auto bytes = std::make_shared<const std::vector<uint8_t>>(readBytes(raw.source, maxRawBytes));
        if (const auto existing = dataFiles.find(raw.id); existing != dataFiles.end()) {
            if (!existing->second || *existing->second != *bytes)
                invalid("raw source changed during export");
        } else
            dataFiles.emplace(raw.id, std::move(bytes));
    }
    for (const auto& [id, bytes] : dataFiles) {
        if (!bytes || bytes->size() > maxRawBytes)
            invalid("invalid or oversized raw data file");
        const auto path = "Data/blobs/" + sha256(*bytes) + ".data";
        writeBytes(root / utf8Path(path), *bytes);
        dataPaths.emplace(id, path);
    }
    auto snapshot = scene.snapshot();
    snapshot.assetRoot.clear();
    const auto scenePath = "Data/scenes/" + snapshot.id.string() + ".scene.json";
    writeText(root / utf8Path(scenePath), encodeScene(Scene::fromSnapshot(snapshot, scene.assets)));
    const auto resources = resourceRecords(scene, modelPaths, dataPaths, scenePath);
    auto additional = metadata.additionalAssets;
    const auto additionalCount = additional.size();
    uniqueIds(additional);
    if (additional.size() != additionalCount)
        invalid("duplicate additional asset");
    for (const auto id : additional)
        if (!resources.contains(id))
            invalid("additional asset is missing: " + id.string());
    Files files;
    std::set<std::wstring> names;
    RuntimePackageStats stats;
    for (const auto& path : listFiles(root)) {
        if (!names.insert(fold(utf8Path(path))).second)
            invalid("case-alias package paths");
        const auto bytes = readBytes(root / utf8Path(path));
        if (bytes.size() > maxPackageBytes - stats.bytes)
            invalid("package exceeds its byte limit");
        stats.bytes += bytes.size();
        files.emplace(path, FileRecord{path, sha256(bytes), bytes.size()});
    }
    requiredFiles(files, metadata.executableName, scenePath);
    (void)decodePlayerGraphics(readText(root / "Config/graphics.json", 64 * 1024));
    std::string fileJson{"["}, resourceJson{"["};
    for (const auto& [path, file] : files) {
        if (fileJson.size() > 1) fileJson += ',';
        fileJson += "{\"path\":" + jsonString(path) + ",\"sha256\":" + jsonString(file.hash) +
                    ",\"bytes\":" + std::to_string(file.bytes) + "}";
    }
    for (const auto& [id, resource] : resources) {
        if (resourceJson.size() > 1) resourceJson += ',';
        resourceJson += "{\"id\":" + jsonString(id.string()) + ",\"type\":" + jsonString(resource.type) +
                        ",\"path\":" + jsonString(resource.path) + ",\"contentHash\":" +
                        jsonString(files.at(resource.path).hash) + ",\"dependencies\":" +
                        idsJson(resource.dependencies) + "}";
    }
    const auto manifest = "{\"format\":\"proto.runtime\",\"formatVersion\":1,\"target\":\"windows-x64\","
        "\"projectId\":" + jsonString(metadata.projectId.string()) + ",\"name\":" + jsonString(metadata.name) +
        ",\"sdkBuildId\":" + jsonString(metadata.sdkBuildId) + ",\"executable\":" + jsonString(metadata.executableName) +
        ",\"startupScene\":" + jsonString(snapshot.id.string()) + ",\"scene\":" + jsonString(scenePath) +
        ",\"graphics\":\"Config/graphics.json\",\"shaders\":\"Data/shaders\",\"models\":" + assetsJson(modelPaths) +
        ",\"rawAssets\":" + assetsJson(dataPaths) + ",\"additionalAssets\":" + idsJson(additional) +
        ",\"resources\":" + resourceJson + "],\"files\":" + fileJson + "]}";
    writeText(root / "Data/runtime.json", manifest);
    stats.files = files.size() + 1;
    stats.bytes += manifest.size();
    stats.models = modelPaths.size();
    stats.rawAssets = dataPaths.size();
    stats.resources = resources.size();
    return stats;
}

RuntimePackage loadRuntimePackage(const fs::path& input, std::string_view expectedBuildId) {
    auto root = fs::absolute(input).lexically_normal();
    ordinary(root);
    const auto attributes = GetFileAttributesW(native(root).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        invalid("package location is missing");
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (fold(root.filename()) != L"runtime.json" || fold(root.parent_path().filename()) != L"data")
            invalid("expected package root or Data/runtime.json");
        root = root.parent_path().parent_path();
    }
    const auto manifestPath = root / "Data/runtime.json";
    auto pins = std::make_shared<SnapshotPins>();
    pins->addDirectory(fs::path(native(root)));
    ordinary(root / "Config");
    pins->addDirectory(fs::path(native(root / "Config")));
    ordinary(manifestPath, true);
    pins->add(fs::path(native(manifestPath)));
    const auto manifestText = readText(manifestPath, 32 * 1024 * 1024);
    JsonDoc doc(manifestText);
    auto* object = doc.root();
    keys(object, {"format", "formatVersion", "target", "projectId", "name", "sdkBuildId", "executable",
                  "startupScene", "scene", "graphics", "shaders", "models", "rawAssets", "additionalAssets",
                  "resources", "files"});
    if (str(get(object, "format")) != "proto.runtime" || integer(get(object, "formatVersion"), 1) != 1 ||
        str(get(object, "target")) != "windows-x64" || str(get(object, "sdkBuildId")) != expectedBuildId)
        invalid("incompatible package/SDK");
    const auto projectId = AssetId::parse(str(get(object, "projectId")));
    const auto name = str(get(object, "name"));
    textField(name, 512);
    const auto executableName = str(get(object, "executable"));
    const auto executable = relative(executableName);
    if (executable.has_parent_path() || fold(executable.extension()) != L".exe")
        invalid("invalid executable name");
    const auto sceneId = AssetId::parse(str(get(object, "startupScene")));
    const auto scenePath = str(get(object, "scene"));
    if (scenePath != "Data/scenes/" + sceneId.string() + ".scene.json" ||
        str(get(object, "graphics")) != "Config/graphics.json" || str(get(object, "shaders")) != "Data/shaders")
        invalid("invalid data/config layout");
    auto* fileList = get(object, "files");
    array(fileList);
    Files files;
    std::set<std::wstring> names;
    RuntimePackageStats stats;
    size_t i{}, n{};
    Json* item{};
    yyjson_arr_foreach(fileList, i, n, item) {
        keys(item, {"path", "sha256", "bytes"});
        const auto path = str(get(item, "path"));
        const auto local = relative(path);
        const auto hash = str(get(item, "sha256"));
        hashText(hash);
        const auto size = integer(get(item, "bytes"), maxFileBytes);
        if (!names.insert(fold(local)).second || !files.emplace(path, FileRecord{path, hash, size}).second)
            invalid("duplicate immutable file");
        if (size > maxPackageBytes - stats.bytes)
            invalid("package exceeds its byte limit");
        stats.bytes += static_cast<size_t>(size);
        const auto full = resolve(root, path);
        pins->add(fs::path(native(full)));
        if (fileSize(full) != size)
            invalid("file size mismatch: " + path);
    }
    requiredFiles(files, executableName, scenePath);
    const auto onDisk = listFiles(root);
    if (onDisk.size() != files.size() ||
        !std::equal(onDisk.begin(), onDisk.end(), files.begin(),
                    [](const std::string& path, const auto& entry) { return path == entry.first; }))
        invalid("immutable files differ from the manifest inventory");
    std::set<std::string> checked;
    const auto verified = [&](const std::string& path, size_t limit = maxFileBytes) {
        const auto found = files.find(path);
        if (found == files.end())
            invalid("resource is outside immutable inventory");
        const auto bytes = readBytes(resolve(root, path), limit);
        if (bytes.size() != found->second.bytes || sha256(bytes) != found->second.hash)
            invalid("content hash mismatch: " + path);
        checked.insert(path);
        return bytes;
    };
    const auto graphicsBytes = verified("Config/graphics.json", 64 * 1024);
    (void)decodePlayerGraphics(std::string(graphicsBytes.begin(), graphicsBytes.end()));
    const auto modelPaths = assetList(get(object, "models"));
    const auto dataPaths = assetList(get(object, "rawAssets"));
    auto catalog = std::make_shared<AssetCatalog>();
    for (const auto& [id, path] : modelPaths) {
        const auto found = files.find(path);
        if (found == files.end() || path != "Data/blobs/" + found->second.hash + ".model")
            invalid("model blob is not content-addressed");
        auto model = decodePortableModel(verified(path));
        if (model->id != id || !model->source.empty())
            invalid("model ID or authored path mismatch");
        catalog->publish(std::move(model));
    }
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> rawPayloads;
    uint64_t residentRawBytes{};
    for (const auto& [id, path] : dataPaths) {
        const auto found = files.find(path);
        if (found == files.end() || path != "Data/blobs/" + found->second.hash + ".data")
            invalid("raw blob is not content-addressed");
        auto [cached, fresh] = rawPayloads.try_emplace(path);
        if (fresh) {
            if (found->second.bytes > maxPackageBytes - residentRawBytes)
                invalid("resident raw assets exceed the package byte limit");
            residentRawBytes += found->second.bytes;
            cached->second = std::make_shared<const std::vector<uint8_t>>(verified(path, maxRawBytes));
        }
        catalog->dataFiles.emplace(id, cached->second);
    }
    const auto sceneBytes = verified(scenePath, 32 * 1024 * 1024);
    auto scene = decodeScene(std::string(sceneBytes.begin(), sceneBytes.end()), std::move(catalog));
    if (scene.snapshot().id != sceneId || !scene.assetRoot.empty())
        invalid("startup scene identity or authored asset root mismatch");
    const auto expectedResources = resourceRecords(scene, modelPaths, dataPaths, scenePath);
    Records declared;
    auto* resources = get(object, "resources");
    array(resources);
    yyjson_arr_foreach(resources, i, n, item) {
        keys(item, {"id", "type", "path", "contentHash", "dependencies"});
        const auto id = AssetId::parse(str(get(item, "id")));
        const auto path = str(get(item, "path"));
        const auto hash = str(get(item, "contentHash"));
        const auto file = files.find(path);
        if (file == files.end() || file->second.hash != hash)
            invalid("resource hash disagrees with file inventory");
        insert(declared, {id, str(get(item, "type")), path, idList(get(item, "dependencies"))});
    }
    if (declared != expectedResources)
        invalid("resource identity/type/dependency table disagrees with payload");
    for (const auto id : idList(get(object, "additionalAssets")))
        if (!declared.contains(id))
            invalid("additional asset is missing");
    for (const auto& [path, record] : files) {
        (void)record;
        if (!checked.contains(path)) {
            const auto bytes = verified(path);
            if (path.starts_with("Data/shaders/") &&
                (bytes.size() < 20 || bytes.size() % 4 || bytes[0] != 3 || bytes[1] != 2 ||
                 bytes[2] != 35 || bytes[3] != 7))
                invalid("invalid SPIR-V shader: " + path);
        }
    }
    ordinary(root / "Config/graphics.user.json");
    stats.files = files.size() + 1;
    stats.bytes += manifestText.size();
    stats.models = modelPaths.size();
    stats.rawAssets = dataPaths.size();
    stats.resources = declared.size();
    return {std::move(scene), root, root / "Data/shaders", root / "Config/graphics.json",
            root / "Config/graphics.user.json", projectId, name, stats, std::move(pins)};
}
} // namespace proto
