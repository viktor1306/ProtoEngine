#pragma once
#include "assets/AssetTypes.hpp"
#include <filesystem>
#include <future>
#include <string>

namespace proto {
struct PreparedMaterialEdit {
    std::string path;
    std::string before;
    std::string after;
};
class AssetWorkspace {
  public:
    explicit AssetWorkspace(std::filesystem::path root);
    const std::filesystem::path& root() const { return root_; }
    std::shared_ptr<const ModelBundle> importFile(const std::filesystem::path& file);
    std::shared_ptr<const ModelBundle> load(AssetId source);
    std::vector<std::pair<AssetId, std::string>> rebuildRegistry();
    PreparedMaterialEdit prepareMaterial(const MaterialAsset& material);
    void saveMaterial(const MaterialAsset& material);

  private:
    std::shared_ptr<const ModelBundle> cook(const std::filesystem::path& source, bool importing);
    std::filesystem::path root_;
};
class ImportJob {
  public:
    bool busy() const { return future_.valid(); }
    void start(std::shared_ptr<AssetWorkspace> workspace, std::filesystem::path source);
    void reload(std::shared_ptr<AssetWorkspace> workspace, AssetId source);
    std::shared_ptr<const ModelBundle> take();

  private:
    std::future<std::shared_ptr<const ModelBundle>> future_;
};
std::vector<ImageMip> makeMips(uint32_t width, uint32_t height, std::vector<uint8_t> rgba, bool srgb, bool normal,
                               float maskCutoff = -1);
void saveCooked(const std::filesystem::path& file, const ModelBundle& bundle);
std::shared_ptr<ModelBundle> readCooked(const std::filesystem::path& file);
} // namespace proto
