#pragma once
#include "behavior/BehaviorSchema.hpp"
#include "runtime/RuntimePackage.hpp"
#include "scene/Scene.hpp"
#include <filesystem>
#include <span>
#include <string>
#include <vector>
namespace proto {
// Resolve behavior-only dependencies into a private snapshot catalog; no entities are instantiated.
void preparePlayAssets(Scene& snapshot, const std::filesystem::path& projectRoot, const BehaviorSchema& schema);

// Resolve the explicit roots from a validated project build manifest in
// addition to typed behavior AssetRefs.  Model/subasset roots load their
// owning ModelBundle; raw files are returned for the portable package writer
// and, when supported by the catalog, are also made resident for Player code.
struct PlayAssetClosure {
    std::vector<AssetId> resolvedRoots;
    std::vector<PackageRawAsset> rawAssets;
};

PlayAssetClosure preparePlayAssets(Scene& snapshot, const std::filesystem::path& projectRoot,
                                   const BehaviorSchema& schema, std::span<const std::string> additionalRoots);
} // namespace proto
