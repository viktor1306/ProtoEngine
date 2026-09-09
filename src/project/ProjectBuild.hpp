#pragma once

#include "behavior/BehaviorSchema.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

enum class BuildDiagnosticLevel { Info, Warning, Error };

struct BuildDiagnostic {
    BuildDiagnosticLevel level{BuildDiagnosticLevel::Info};
    std::string stage;
    std::string message;
    std::optional<std::string> sourcePath;
    std::optional<unsigned> line;
    std::optional<unsigned> column;
};

struct BuildProgress {
    std::string stage;
    std::string message;
};

using BuildProgressCallback = std::function<void(const BuildProgress&)>;

struct ProjectBuildRequest {
    std::filesystem::path projectRoot;
    std::filesystem::path sdkRoot;
    // Empty selects the package's standard configuration from sdk.json.
    std::string configuration;

    // M4 projects use these fixed paths.  They are retained in the request so
    // callers can report a useful error when an older/malformed manifest is
    // handed to the build service; values other than the fixed contract are
    // rejected before any generated files are written.
    std::string sourceRoot{"Code"};
    std::string registrationFile{"Code/Behaviors.cpp"};

    size_t maxSourceFileBytes{8 * 1024 * 1024};
    size_t maxSourceBytes{64 * 1024 * 1024};
    size_t processOutputLimit{4 * 1024 * 1024};
    std::chrono::milliseconds configureTimeout{std::chrono::minutes(2)};
    std::chrono::milliseconds buildTimeout{std::chrono::minutes(10)};
    std::chrono::milliseconds describeTimeout{std::chrono::seconds(5)};
    unsigned parallelism{4};
    std::string expectedSdkBuildId;
};

struct ProjectBuildResult {
    std::filesystem::path executable;
    std::string key;
    BehaviorSchema schema;
    std::string describeJson;
    std::vector<BuildDiagnostic> diagnostics;
};

class ProjectBuildError final : public std::runtime_error {
  public:
    ProjectBuildError(std::string message, std::vector<BuildDiagnostic> diagnostics = {});
    const std::vector<BuildDiagnostic>& diagnostics() const noexcept { return diagnostics_; }

  private:
    std::vector<BuildDiagnostic> diagnostics_;
};

// Build a project using only its Code snapshot and the installed SDK package.
// The generated CMake project is internal to .proto/builds/<key>; authored
// files and user CMake files are never executed.  A result is returned only
// after compile/link, same-executable --describe-behaviors, and a final Code
// content check all succeed.
ProjectBuildResult buildProject(const ProjectBuildRequest& request, std::atomic_bool& cancel,
                                BuildProgressCallback progress = {});

} // namespace proto
