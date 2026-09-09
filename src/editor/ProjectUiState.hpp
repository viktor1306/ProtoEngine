#pragma once
#include "project/AssetFilePlanner.hpp"
#include <imgui.h>
#include <glm/vec3.hpp>
#include <array>
#include <functional>
#include <map>
#include <set>

namespace proto {
struct ProjectUiState {
    struct Entry {
        std::string path, name, kind, assetId;
        bool directory{}, protectedPath{};
        uint64_t size{};
    };
    std::string directory;
    std::vector<Entry> entries;
    std::vector<ProjectAsset> assets;
    std::set<std::string> selection;
    std::vector<std::string> clipboard;
    bool cut{}, focused{}, refreshPending{}, createPopup{}, renamePopup{}, folderPopup{}, deletePopup{}, copyAs{};
    std::filesystem::path createParent;
    std::string renamePath;
    std::array<char, 256> name{}, filter{};
    std::function<void()> pending;
    double changedAt{};
    uint64_t refreshes{};
    std::map<std::string, ImVec2> rowCenters;
    ImVec2 pasteCenter{}, backCenter{};
};
struct ProjectSmokeState {
    std::filesystem::path root, model;
    int stage{};
    uint64_t frame{}, refreshBaseline{};
    bool passed{};
    AssetId projectId, sceneId, modelId, copiedId;
    EntityId entity;
    std::string source, renamed, copied, scenePath;
    size_t history{};
    glm::vec3 editedPosition{};
};
} // namespace proto
