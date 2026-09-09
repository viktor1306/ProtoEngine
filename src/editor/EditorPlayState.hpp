#pragma once
#include "project/ProjectBuild.hpp"
#include "project/ProjectPackage.hpp"
#include "project/ManagedProcess.hpp"
#include "runtime/RuntimeSnapshot.hpp"
#include "core/Diagnostics.hpp"
#include <future>
#include <mutex>

namespace proto {
enum class PlayPhase { Editing, Preparing, Starting, Running, Stopping };
struct PlayPreparation {
    ProjectBuildResult build;
    std::optional<PlaySnapshot> snapshot;
    std::string sceneError;
};
struct PlayUiState {
    PlayPhase phase{PlayPhase::Editing};
    bool launch{}, showLog{true}, forced{}, lastSucceeded{}, sessionLaunched{};
    std::shared_ptr<std::atomic_bool> cancel{std::make_shared<std::atomic_bool>(false)};
    std::future<PlayPreparation> preparation;
    std::future<ProjectPackageResult> packaging;
    std::optional<ProjectPackageResult> lastPackage;
    std::mutex logMutex;
    std::string log;
    std::vector<BuildDiagnostic> diagnostics;
    std::optional<BehaviorSchema> schema;
    std::optional<PlaySnapshot> snapshot;
    ManagedProcess player;
    void* ready{};
    std::wstring readyName;
    Clock::time_point phaseStarted{}, metricsAt{};
    ProcessStats playerStats, editorStats;
    double playerCpuPercent{}, editorCpuPercent{};
    uint64_t pausedSceneFrames{};
    std::vector<std::string> extraPlayerArguments;
    ~PlayUiState();
    void append(std::string_view text);
};
} // namespace proto
