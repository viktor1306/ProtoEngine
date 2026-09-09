#include "project/PlayAssets.hpp"

#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "project/AssetFilePlanner.hpp"

#include <windows.h>

#include <algorithm>
#include <concepts>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace proto {
namespace {
namespace fs = std::filesystem;

template <class Catalog>
concept HasResidentRawFiles = requires(Catalog& catalog, AssetId id, std::shared_ptr<const std::vector<uint8_t>> bytes) {
    catalog.dataFiles[id] = bytes;
};

template <class Catalog>
bool hasResidentRawFile(const Catalog& catalog, AssetId id) {
    if constexpr (HasResidentRawFiles<Catalog>)
        return catalog.dataFiles.contains(id);
    else {
        (void)catalog;
        (void)id;
        return false;
    }
}

template <class Catalog>
void publishResidentRawFile(Catalog& catalog, AssetId id, std::vector<uint8_t> bytes) {
    if constexpr (HasResidentRawFiles<Catalog>)
        catalog.dataFiles[id] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    else {
        (void)catalog;
        (void)id;
        (void)bytes;
    }
}

std::string lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

struct AssetLookup {
    std::vector<ProjectAsset> assets;
    std::unordered_map<std::string, const ProjectAsset*> byId;
    std::unordered_map<std::string, const ProjectAsset*> byPath;
};

AssetLookup makeLookup(const std::filesystem::path& projectRoot) {
    AssetLookup lookup;
    lookup.assets = inspectProjectAssets(projectRoot);
    for (const auto& asset : lookup.assets) {
        if (!asset.id.empty())
            lookup.byId.emplace(lower(asset.id), &asset);
        if (!asset.path.empty()) {
            const auto key = projectPathKey(asset.path);
            const auto found = lookup.byPath.find(key);
            // inspectProjectAssets exposes a ModelSource and its subassets at
            // the same source path. A path root must resolve to the authored
            // model ID; choosing the first subasset would publish the wrong
            // additionalAssets identity even though the owner bundle loads.
            if (found == lookup.byPath.end() ||
                (found->second->kind != "ModelSource" && asset.kind == "ModelSource"))
                lookup.byPath[key] = &asset;
        }
    }
    return lookup;
}

bool isModelKind(std::string_view kind) {
    return kind == "ModelSource" || kind == "Mesh" || kind == "Material" || kind == "Texture";
}

const ProjectAsset* findAsset(const AssetLookup& lookup, std::string_view root) {
    try {
        const auto id = AssetId::parse(root);
        if (const auto found = lookup.byId.find(lower(id.string())); found != lookup.byId.end())
            return found->second;
        return nullptr;
    } catch (const std::exception&) {
    }
    if (root.empty())
        throw std::runtime_error("additionalAssets contains an empty root");
    const auto key = projectPathKey(std::string(root));
    if (const auto found = lookup.byPath.find(key); found != lookup.byPath.end())
        return found->second;
    return nullptr;
}

std::filesystem::path checkedProjectFile(const std::filesystem::path& projectRoot, const std::string& relative) {
    if (!projectVisible(relative))
        throw std::runtime_error("additionalAssets must name an authored runtime file: " + relative);
    const auto path = projectPath(projectRoot, relative, false);
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("additionalAssets contains a reparse point: " + relative);
    if (!fs::is_regular_file(path))
        throw std::runtime_error("additionalAssets file is missing or not regular: " + relative);
    return path;
}

AssetId syntheticRawId(const std::filesystem::path& projectRoot, const std::string& relative) {
    // A plain file has no authored AssetId. This fallback is only used by
    // direct callers that bypass ProjectSession's sidecar registration; keep
    // it independent of an absolute root so relocation does not rewrite it.
    const auto digest = sha256(projectPathKey(relative));
    AssetId result;
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<uint8_t>(c - '0');
        return static_cast<uint8_t>(c - 'a' + 10);
    };
    for (size_t i = 0; i != result.uuid.bytes.size(); ++i)
        result.uuid.bytes[i] = static_cast<uint8_t>((nibble(digest[i * 2]) << 4) | nibble(digest[i * 2 + 1]));
    result.uuid.bytes[6] = static_cast<uint8_t>((result.uuid.bytes[6] & 0x0f) | 0x40);
    result.uuid.bytes[8] = static_cast<uint8_t>((result.uuid.bytes[8] & 0x3f) | 0x80);
    (void)projectRoot;
    return result;
}

void addRaw(PlayAssetClosure& closure, Scene& scene, AssetId id, const fs::path& path) {
    for (const auto& raw : closure.rawAssets) {
        std::error_code error;
        if (raw.id == id && fs::equivalent(raw.source, path, error) && !error)
            return;
    }
    auto bytes = assetBytes(path, 512 * 1024 * 1024);
    publishResidentRawFile(*scene.assets, id, bytes);
    closure.rawAssets.push_back({id, path});
}

void addModel(Scene& scene, const std::shared_ptr<AssetWorkspace>& workspace, AssetId owner) {
    if (!workspace)
        throw std::runtime_error("Project asset workspace is unavailable");
    if (scene.assets->models.contains(owner))
        return;
    const auto model = workspace->load(owner);
    if (!model || model->id != owner)
        throw std::runtime_error("Model dependency has an invalid owner: " + owner.string());
    scene.assets->publish(model);
}

void resolveAsset(PlayAssetClosure& closure, Scene& scene, const fs::path& projectRoot, const AssetLookup& lookup,
                  const std::shared_ptr<AssetWorkspace>& workspace, const ProjectAsset& asset) {
    const auto kind = std::string_view(asset.kind);
    if (isModelKind(kind)) {
        const auto owner = kind == "ModelSource" ? AssetId::parse(asset.id) : AssetId::parse(asset.owner);
        addModel(scene, workspace, owner);
        return;
    }
    if (kind == "RawDependency" || kind == "RawFile") {
        if (asset.id.empty())
            throw std::runtime_error("Raw dependency has no AssetId: " + asset.path);
        addRaw(closure, scene, AssetId::parse(asset.id), checkedProjectFile(projectRoot, asset.path));
        return;
    }
    if (kind == "Scene")
        throw std::runtime_error("A scene cannot be an additional runtime asset: " + asset.path);
    if (!asset.id.empty())
        addRaw(closure, scene, AssetId::parse(asset.id), checkedProjectFile(projectRoot, asset.path));
    else
        throw std::runtime_error("Unsupported additional asset kind: " + asset.kind);
    (void)lookup;
}

void resolveRoot(PlayAssetClosure& closure, Scene& scene, const fs::path& projectRoot, const AssetLookup& lookup,
                 const std::shared_ptr<AssetWorkspace>& workspace, const std::string& root) {
    const auto found = findAsset(lookup, root);
    if (found) {
        resolveAsset(closure, scene, projectRoot, lookup, workspace, *found);
        const auto id = AssetId::parse(found->id);
        if (std::find(closure.resolvedRoots.begin(), closure.resolvedRoots.end(), id) == closure.resolvedRoots.end())
            closure.resolvedRoots.push_back(id);
        return;
    }

    // A relative path without a sidecar is an explicitly requested immutable
    // raw file. projectPath performs traversal/absolute-path validation.
    const auto path = checkedProjectFile(projectRoot, root);
    const auto id = syntheticRawId(projectRoot, root);
    addRaw(closure, scene, id, path);
    closure.resolvedRoots.push_back(id);
}

void resolveBehaviorReferences(PlayAssetClosure& closure, Scene& scene, const fs::path& projectRoot,
                               const BehaviorSchema& schema, const AssetLookup& lookup,
                               const std::shared_ptr<AssetWorkspace>& workspace) {
    std::set<AssetId> required;
    for (const auto handle : scene.entities()) {
        for (const auto& binding : scene.behaviors(handle)) {
            const auto* descriptor = schema.find(binding.type);
            if (!descriptor)
                throw std::runtime_error("Missing behavior type: " + binding.type.string());
            for (const auto& [name, value] : normalizeBehaviorProperties(*descriptor, binding.properties)) {
                (void)name;
                if (const auto* ref = std::get_if<sdk::AssetRef>(&value); ref && ref->id)
                    required.insert(ref->id);
            }
        }
    }

    const auto present = [&](AssetId id) {
        return id == builtin::cube || id == builtin::plane || scene.assets->models.contains(id) ||
               scene.assets->meshes.contains(id) || scene.assets->materials.contains(id) ||
               scene.assets->textures.contains(id) || hasResidentRawFile(*scene.assets, id);
    };
    for (const auto id : required) {
        if (present(id))
            continue;
        const auto found = lookup.byId.find(lower(id.string()));
        if (found == lookup.byId.end())
            throw std::runtime_error("Missing behavior AssetRef: " + id.string());
        resolveAsset(closure, scene, projectRoot, lookup, workspace, *found->second);
        if (!present(id))
            throw std::runtime_error("Behavior AssetRef is not in the runtime closure: " + id.string());
    }
}

bool behaviorReferencesAlreadyPresent(const Scene& scene, const BehaviorSchema& schema) {
    const auto present = [&](AssetId id) {
        return id == builtin::cube || id == builtin::plane || scene.assets->models.contains(id) ||
               scene.assets->meshes.contains(id) || scene.assets->materials.contains(id) ||
               scene.assets->textures.contains(id) || hasResidentRawFile(*scene.assets, id);
    };
    for (const auto handle : scene.entities()) {
        for (const auto& binding : scene.behaviors(handle)) {
            const auto* descriptor = schema.find(binding.type);
            if (!descriptor)
                throw std::runtime_error("Missing behavior type: " + binding.type.string());
            for (const auto& [name, value] : normalizeBehaviorProperties(*descriptor, binding.properties)) {
                (void)name;
                if (const auto* ref = std::get_if<sdk::AssetRef>(&value); ref && ref->id && !present(ref->id))
                    return false;
            }
        }
    }
    return true;
}

} // namespace

void preparePlayAssets(Scene& scene, const std::filesystem::path& projectRoot, const BehaviorSchema& schema) {
    (void)preparePlayAssets(scene, projectRoot, schema, {});
}

PlayAssetClosure preparePlayAssets(Scene& scene, const std::filesystem::path& projectRoot,
                                   const BehaviorSchema& schema, std::span<const std::string> additionalRoots) {
    scene.assets = std::make_shared<AssetCatalog>(*scene.assets);
    if (additionalRoots.empty() && behaviorReferencesAlreadyPresent(scene, schema))
        return {};
    const auto lookup = makeLookup(projectRoot);
    const auto workspace = std::make_shared<AssetWorkspace>(projectRoot);
    PlayAssetClosure closure;

    // Startup model sources are authoritative scene dependencies. They are
    // loaded before behavior/additional closure resolution and never inferred
    // by searching C++ source text.
    for (const auto id : scene.modelSources)
        addModel(scene, workspace, id);

    resolveBehaviorReferences(closure, scene, projectRoot, schema, lookup, workspace);
    for (const auto& root : additionalRoots)
        resolveRoot(closure, scene, projectRoot, lookup, workspace, root);
    scene.assetsChanged();
    return closure;
}

} // namespace proto
