#pragma once

#include "runtime/RuntimeSnapshot.hpp"
#include <span>

namespace proto {

struct PackageRawAsset {
    AssetId id;
    std::filesystem::path source;
};

struct RuntimePackageMetadata {
    AssetId projectId;
    std::string name;
    std::string sdkBuildId;
    std::string executableName;
    std::vector<AssetId> additionalAssets;
};

struct RuntimePackageStats {
    size_t files{}, bytes{}, models{}, rawAssets{}, resources{};
};

struct RuntimePackage {
    Scene scene;
    std::filesystem::path root, shaderDirectory, graphicsPath, userGraphicsPath;
    AssetId projectId;
    std::string name;
    RuntimePackageStats stats;
    std::shared_ptr<SnapshotPins> pins;
};

// Called in a new owned staging directory after copying the executable, shaders,
// Config/graphics.json and notices/licenses. Writes portable data and a complete
// immutable-file manifest. The supplied catalog must contain only its closure.
RuntimePackageStats writeRuntimePackage(const std::filesystem::path& packageRoot, const Scene& scene,
                                       const RuntimePackageMetadata& metadata,
                                       std::span<const PackageRawAsset> rawAssets = {});

// Accepts Data/runtime.json or the package root. Resolves all immutable files
// below that root, checks hashes/types/references, and never opens authored data.
// Retain the returned pins for the lifetime of the running Player.
RuntimePackage loadRuntimePackage(const std::filesystem::path& rootOrManifest, std::string_view expectedBuildId);

} // namespace proto
