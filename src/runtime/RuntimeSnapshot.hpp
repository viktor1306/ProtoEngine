#pragma once
#include "scene/Scene.hpp"
#include <filesystem>

namespace proto {
// File handles deny writers/deletion for the duration of a Play session.
class SnapshotPins {
  public:
    SnapshotPins() = default;
    ~SnapshotPins();
    SnapshotPins(const SnapshotPins&) = delete;
    SnapshotPins& operator=(const SnapshotPins&) = delete;
    void add(const std::filesystem::path& file);
    void addDirectory(const std::filesystem::path& directory);

  private:
    std::vector<void*> handles_;
};
struct PlaySnapshot {
    std::filesystem::path manifest;
    std::shared_ptr<SnapshotPins> pins;
    std::string sceneHash;
    size_t blobs{}, bytes{};
};
PlaySnapshot stagePlaySnapshot(const std::filesystem::path& projectRoot, const Scene& scene,
                               std::string_view sdkBuildId);
Scene loadRuntimeSnapshot(const std::filesystem::path& manifest, std::string_view expectedBuildId);
} // namespace proto
