#pragma once
#include <filesystem>
namespace proto::fixtures {
// Deterministic, original test assets. No external model downloads.
void create(const std::filesystem::path& directory);
} // namespace proto::fixtures
