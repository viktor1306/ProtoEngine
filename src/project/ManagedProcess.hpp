#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

// A child process is always started with an explicit executable and argument
// vector.  ProcessSpec does not expose CreateProcess flags or inheritable
// handles: the implementation owns all process, pipe and job handles.
struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    size_t stdoutLimit{4 * 1024 * 1024};
    size_t stderrLimit{4 * 1024 * 1024};
    // Remove ambient compiler/toolchain variables for deterministic CMake
    // configure/build children.  Other processes inherit the normal editor
    // environment.
    bool clearToolchainEnvironment{};
    // For compiler children only: resolve GCC's companion as/ld from the
    // selected SDK toolchain, independently of the launching terminal PATH.
    // This is applied to the child environment and never changes the parent.
    std::filesystem::path toolchainDirectory;
    // Failure injection for suspended-start cleanup tests; production leaves this empty.
    std::function<void(uint32_t)> diagnosticBeforeJobAssignment;
};

struct ProcessStats {
    double cpuSeconds{};
    uint64_t workingSetBytes{};
};

struct ProcessPoll {
    bool running{};
    std::optional<uint32_t> exitCode;
    size_t stdoutBytesRead{};
    size_t stderrBytesRead{};
    bool stdoutTruncated{};
    bool stderrTruncated{};
};

// ManagedProcess owns a suspended child and a kill-on-close job.  The child
// is resumed only after it has been assigned to that job.  poll() drains both
// redirected pipes without waiting on a pipe read, and is safe to call from an
// asynchronous build/play loop.
class ManagedProcess final {
  public:
    ManagedProcess();
    ~ManagedProcess();
    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;
    ManagedProcess(ManagedProcess&&) noexcept;
    ManagedProcess& operator=(ManagedProcess&&) noexcept;

    // Creates a unique manual-reset stop event owned by this process object.
    // Call before start() when the event name must be passed as a child
    // argument.  The event remains signalled until resetStop() or destruction.
    std::wstring prepareStopEvent(std::wstring_view prefix = L"ProtoPlayer.Stop");
    void resetStop();
    void signalStop();
    const std::wstring& stopEventName() const noexcept;

    void start(const ProcessSpec& spec);
    ProcessPoll poll();
    // Remains true until poll() observes exit and drains final output.
    bool running() const noexcept;
    std::optional<uint32_t> exitCode() const noexcept;

    // Returns and clears output accumulated since the previous call.  Output
    // is retained only up to the per-stream limits from ProcessSpec.
    std::string takeStdout();
    std::string takeStderr();
    bool stdoutTruncated() const noexcept;
    bool stderrTruncated() const noexcept;

    // Terminates only this process's owned job tree.  It is idempotent and
    // returns after the root process has exited (or the bounded wait expires).
    void forceStop(std::chrono::milliseconds wait = std::chrono::milliseconds(2000));
    ProcessStats stats() const noexcept;
    uint32_t processId() const noexcept;

  private:
    struct State;
    State* state_{};
};

} // namespace proto
