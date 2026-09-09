#pragma once

#include "project/FileTransactions.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace proto {

// Operations understood by the project file browser.  The planner only reads
// the project and returns a description; FileTransaction owns all publication.
enum class FileOperation { Move, Copy, Remove };

struct ProjectAsset {
    std::string id;
    std::string path;
    std::string kind;
    std::string owner;
    std::optional<std::string> sourcePath;
};

// Build a conflict-guarded, filesystem-only plan.  sourcePaths and
// destinationDirectory are project-relative UTF-8 paths using '/'.  A model's
// .meta and URI dependencies are enrolled implicitly; .meta is never a direct
// browser target.  No filesystem mutation happens in these functions.
FilePlan planProjectFiles(const std::filesystem::path& root, FileOperation operation,
                          const std::vector<std::string>& sourcePaths, const std::string& destinationDirectory,
                          const std::optional<std::string>& newName = {});

// Enroll authored scene/raw-dependency sidecars that are missing and enrich
// ModelSource dependency records with stable raw-dependency IDs and JSON URI
// pointers.  Existing bytes are guarded and preserved unless enrichment is
// required.  The service tree, code, and project control files are ignored.
FilePlan enrollProjectFiles(const std::filesystem::path& root);

// Return the project asset index reconstructed from authored files.  The
// registry/cache/service tree is deliberately excluded.
std::vector<ProjectAsset> inspectProjectAssets(const std::filesystem::path& root);

} // namespace proto
