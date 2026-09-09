#pragma once
#include "scene/Scene.hpp"
#include <filesystem>
#include <string_view>

namespace proto {
std::string readDocument(const std::filesystem::path& path);
Scene decodeScene(std::string_view json, std::shared_ptr<AssetCatalog> assets = {});
std::string encodeScene(const Scene& scene);
// Same-directory staging + flush; preserves the previous contents as .bak.
void atomicWrite(const std::filesystem::path& path, std::string_view bytes, std::optional<std::string_view> expected = {});
}
