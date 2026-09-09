#pragma once

#include "assets/AssetWorkspace.hpp"
#include "core/Id.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace proto {
struct ProjectInfo {
    AssetId id;
    std::string name;
    AssetId startupScene;
};

// The validated, authored build portion of project.proto.json.  Paths in
// additionalAssets are either canonical AssetIds or project-relative UTF-8
// paths; callers may use this value while the session's writer lock is held.
struct ProjectBuildSettings {
    std::string target;
    std::string name;
    std::vector<std::string> additionalAssets;
};

class ProjectSession final {
  public:
    // The target must not exist.  Creation is staged next to the destination
    // and published with a no-overwrite directory move.
    static std::shared_ptr<ProjectSession> create(const std::filesystem::path& root, std::string name);

    // Accepts either a project directory or its project.proto.json manifest.
    static std::shared_ptr<ProjectSession> open(const std::filesystem::path& rootOrManifest);

    ~ProjectSession();
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;

    const std::filesystem::path& root() const noexcept { return root_; }
    const ProjectInfo& info() const noexcept { return info_; }
    const std::filesystem::path& startupScenePath() const noexcept { return startupScenePath_; }
    const ProjectBuildSettings& buildSettings() const noexcept { return buildSettings_; }
    const std::filesystem::path& graphicsSettingsPath() const noexcept { return graphicsSettingsPath_; }
    std::shared_ptr<AssetWorkspace> assetWorkspace() const noexcept { return workspace_; }

    // Re-read the manifest and scene index.  The in-memory state is replaced
    // only after every part of the candidate has validated successfully.
    void refresh();
    struct RefreshCandidate {
        ProjectInfo info;
        std::filesystem::path startupScenePath;
        ProjectBuildSettings buildSettings;
        std::filesystem::path graphicsSettingsPath;
    };
    RefreshCandidate prepareRefresh() const;
    void publishRefresh(RefreshCandidate&& candidate) noexcept;

    // Consume an external-authored change notification without scanning the
    // filesystem.  The next refresh performs the full validation/rescan.
    bool consumeChanged() noexcept;

  private:
    struct Lock;
    ProjectSession(std::filesystem::path root, ProjectInfo info, std::filesystem::path startupScenePath,
                   ProjectBuildSettings buildSettings, std::filesystem::path graphicsSettingsPath,
                   std::shared_ptr<AssetWorkspace> workspace, std::unique_ptr<Lock> lock);

    std::filesystem::path root_;
    ProjectInfo info_;
    std::filesystem::path startupScenePath_;
    ProjectBuildSettings buildSettings_;
    std::filesystem::path graphicsSettingsPath_;
    std::shared_ptr<AssetWorkspace> workspace_;
    std::unique_ptr<Lock> lock_;
    std::unique_ptr<class DirectoryWatcher> watcher_;
};

} // namespace proto
