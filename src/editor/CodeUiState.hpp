#pragma once
#include "project/FileTransactions.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace proto {
struct CodeUiState {
    std::filesystem::path path;
    std::string relative, text, savedText;
    FileStamp stamp;
    std::vector<char> buffer;
    size_t line{};
    bool linePending{}, dirty{}, focused{}, focusRequested{};
    uint64_t widgetRevision{};
};
} // namespace proto
