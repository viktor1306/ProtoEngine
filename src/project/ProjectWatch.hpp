#pragma once

#include <filesystem>
#include <memory>

namespace proto {

// A native, recursive project-directory change source.  The watcher owns its
// worker and only publishes a coalesced changed bit; callers decide when to
// rescan their authored state.
class DirectoryWatcher {
  public:
    explicit DirectoryWatcher(std::filesystem::path root);
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
    DirectoryWatcher(DirectoryWatcher&&) = delete;
    DirectoryWatcher& operator=(DirectoryWatcher&&) = delete;

    // Returns and clears the coalesced notification bit.  This is deliberately
    // non-blocking and safe to call from the editor thread.
    bool consumeChanged() noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// ProjectWatch is the public name used by the project layer.  Keep the native
// implementation's descriptive name available as well for focused tests.
using ProjectWatch = DirectoryWatcher;

} // namespace proto
