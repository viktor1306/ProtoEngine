#pragma once

#include "behavior/BehaviorSchema.hpp"
#include "project/ProjectBuild.hpp"
#include "project/ProjectSession.hpp"
#include "runtime/RuntimePackage.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace proto {

struct PackageDllAudit {
    std::vector<std::string> imported;
    std::vector<std::string> system;
    std::vector<std::string> copied;
};

struct ProjectPackageRequest {
    // The caller owns the editor/session lock for the entire export.  The
    // exporter never reopens this project, so a saved scene and manifest are
    // read from the same validated session.
    std::shared_ptr<ProjectSession> session;
    std::filesystem::path sdkRoot;
    std::filesystem::path outputDirectory;
    std::string expectedSdkBuildId;
    std::string configuration{"Release"};
    size_t processOutputLimit{4 * 1024 * 1024};
    unsigned parallelism{4};
    // Optional test-only fault injection. The exporter calls this with
    // "staged", "before-input-validation", and "before-publish" at the
    // corresponding transaction boundaries; production callers leave it empty.
    std::function<void(std::string_view)> faultHook;
};

struct ProjectPackageResult {
    std::filesystem::path packageRoot;
    std::filesystem::path executable;
    std::filesystem::path manifest;
    std::string key;
    BehaviorSchema schema;
    RuntimePackageStats runtimeStats;
    PackageDllAudit dllAudit;
};

class ProjectPackageError final : public std::runtime_error {
  public:
    ProjectPackageError(std::string message, std::vector<BuildDiagnostic> diagnostics = {})
        : std::runtime_error(std::move(message)), diagnostics_(std::move(diagnostics)) {}
    const std::vector<BuildDiagnostic>& diagnostics() const noexcept { return diagnostics_; }

  private:
    std::vector<BuildDiagnostic> diagnostics_;
};

// Build and publish a standalone package from the saved startup scene.  All
// expensive work happens in a sibling staging directory; the final rename is
// the only operation that changes outputDirectory.  Cancellation before the
// rename discards only that staging tree.
ProjectPackageResult buildProjectPackage(const ProjectPackageRequest& request, std::atomic_bool& cancel,
                                         BuildProgressCallback progress = {});

// Bounded, read-only PE import inspection used by the package audit and its
// focused parser regression. It includes both normal and delay-import tables.
std::vector<std::string> inspectPortableExecutableImports(const std::filesystem::path& executable);

} // namespace proto
