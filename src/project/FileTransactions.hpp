#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace proto {
enum class FileKind { Missing, File, Directory };
struct FileStamp {
    FileKind kind{FileKind::Missing};
    uint64_t size{};
    std::string hash;
    bool operator==(const FileStamp&) const = default;
};
// All authored paths are UTF-8, project-relative and use forward slashes.
// No reparse points, ADS, device paths, Win32 aliases or escapes are accepted.
std::filesystem::path projectPath(const std::filesystem::path& root, const std::string& relative,
                                  bool internal = false);
std::string projectRelative(const std::filesystem::path& root, const std::filesystem::path& path);
std::string projectPathKey(const std::string& relative);
bool projectVisible(const std::string& relative);
FileStamp projectStamp(const std::filesystem::path& root, const std::string& relative);

struct FileEdit {
    std::string path;
    FileKind kind{FileKind::File};
    // File payload: exactly one of inline UTF-8 bytes or another authored path.
    // Large binary files are streamed to the journal, never kept in Undo RAM.
    std::optional<std::string> text;
    std::string source;
};
struct FileGuard {
    std::string path;
    FileStamp stamp;
};
struct PathMove {
    std::string before, after;
};
struct FilePlan {
    std::string label;
    std::vector<FileEdit> edits;
    std::vector<FileGuard> guards;
    std::vector<PathMove> moves;
};

class FileTransaction {
  public:
    static std::shared_ptr<FileTransaction> prepare(const std::filesystem::path& root, const FilePlan& plan);
    static void recover(const std::filesystem::path& root, const std::function<void(size_t)>& afterStep = {});
    ~FileTransaction();
    void redo();
    void undo();
    size_t memoryBytes() const;
    uint64_t diskBytes() const;
    const std::vector<PathMove>& moves() const;
    const std::string& label() const;
    const std::filesystem::path& journalPath() const;
    // Diagnostic injection: called after an authored step has been published
    // but before progress is journaled. Production leaves this callback empty.
    std::function<void(size_t)> afterStep;

  private:
    struct State;
    explicit FileTransaction(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace proto
