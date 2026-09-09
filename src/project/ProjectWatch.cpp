#include "project/ProjectWatch.hpp"

#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "project/FileTransactions.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace proto {
namespace {

struct WinHandle {
    HANDLE value{INVALID_HANDLE_VALUE};

    WinHandle() = default;
    explicit WinHandle(HANDLE handle) : value(handle) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr)
                CloseHandle(value);
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~WinHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
    [[nodiscard]] bool valid() const noexcept { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};

bool ignoredComponent(std::string_view component) {
    std::string key;
    try {
        key = projectPathKey(std::string(component));
    } catch (...) {
        return false;
    }
    return key == ".PROTO" || key.ends_with(".BAK") || key.ends_with(".LOCK") || key.find(".TMP-") != std::string::npos;
}

bool ignoredPath(std::string_view relative) {
    try {
        for (const auto& part : utf8Path(std::string(relative)))
            if (ignoredComponent(utf8(part.generic_wstring())))
                return true;
    } catch (...) {
        // A path that cannot be decoded is not a service file.  Marking it as
        // authored makes refresh validate the project instead of silently
        // losing a notification.
        return false;
    }
    return false;
}

} // namespace

struct DirectoryWatcher::State {
    static constexpr DWORD bufferBytes = 64 * 1024;

    std::filesystem::path root;
    WinHandle directory;
    WinHandle stopEvent;
    WinHandle notifyEvent;
    std::vector<std::byte> buffer{bufferBytes};
    OVERLAPPED overlapped{};
    std::atomic_bool changed{false};
    bool pending{}; // Owned by the worker after construction.
    std::jthread worker;

    explicit State(std::filesystem::path path) : root(std::move(path)) {
        const auto base = std::filesystem::absolute(root).lexically_normal();
        // FileTransactions performs the same no-reparse validation used for
        // every authored project path.  The watcher must never follow a link.
        (void)projectPath(base, "");
        if (!std::filesystem::is_directory(base))
            throw std::runtime_error("Project watch root is not a directory");
        root = base;

        directory = WinHandle(CreateFileW(
            root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!directory.valid())
            throw std::runtime_error("Cannot open project directory watcher");
        stopEvent = WinHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        notifyEvent = WinHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent.valid() || !notifyEvent.valid())
            throw std::runtime_error("Cannot create project watcher events");
        overlapped.hEvent = notifyEvent.value;
        // Arm before returning so an immediate external write cannot fall into
        // the gap before the new worker gets scheduled.
        issueRead();
        try {
            worker = std::jthread([this](std::stop_token) { run(); });
        } catch (...) {
            drain();
            throw;
        }
    }

    ~State() {
        if (stopEvent.valid())
            SetEvent(stopEvent.value);
        if (worker.joinable())
            worker.join();
        // Handles are closed by their RAII members only after the worker has
        // stopped using them.  This ordering is essential for overlapped I/O.
    }

    void signal() noexcept { changed.store(true, std::memory_order_release); }

    void drain() noexcept {
        if (!pending)
            return;
        CancelIoEx(directory.value, &overlapped);
        DWORD bytes{};
        // Cancellation only requests completion. Retain the OVERLAPPED and
        // buffer until the kernel has completed the request (including abort).
        GetOverlappedResult(directory.value, &overlapped, &bytes, TRUE);
        pending = false;
    }

    void issueRead() {
        overlapped = {};
        overlapped.hEvent = notifyEvent.value;
        ResetEvent(notifyEvent.value);
        DWORD bytes{};
        const BOOL accepted = ReadDirectoryChangesW(
            directory.value, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
            &bytes, &overlapped, nullptr);
        if (!accepted && GetLastError() != ERROR_IO_PENDING)
            throw std::runtime_error("Cannot arm project directory watcher");
        pending = true;
    }

    void parse(DWORD bytes) {
        if (bytes == 0) {
            // A zero-byte completion is the documented overflow indication.
            signal();
            return;
        }
        DWORD offset{};
        while (offset < bytes) {
            if (bytes - offset < sizeof(FILE_NOTIFY_INFORMATION)) {
                signal();
                return;
            }
            auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            if (item->FileNameLength % sizeof(wchar_t) != 0 ||
                item->FileNameLength > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
                signal();
                return;
            }
            try {
                const auto relative = utf8(std::wstring_view(item->FileName, item->FileNameLength / sizeof(wchar_t)));
                if (!ignoredPath(relative))
                    signal();
            } catch (...) {
                signal();
            }
            if (item->NextEntryOffset == 0)
                break;
            if (item->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) || item->NextEntryOffset > bytes - offset) {
                signal();
                return;
            }
            offset += item->NextEntryOffset;
        }
    }

    void run() noexcept {
        try {
            const HANDLE events[] = {notifyEvent.value, stopEvent.value};
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_FAILED)
                    break;
                if (wait != WAIT_OBJECT_0)
                    break;
                DWORD bytes{};
                if (!GetOverlappedResult(directory.value, &overlapped, &bytes, FALSE)) {
                    const DWORD error = GetLastError();
                    if (error == ERROR_IO_INCOMPLETE)
                        break;
                    pending = false;
                    if (error == ERROR_OPERATION_ABORTED || WaitForSingleObject(stopEvent.value, 0) == WAIT_OBJECT_0)
                        break;
                    signal();
                } else {
                    pending = false;
                    parse(bytes);
                }
                if (WaitForSingleObject(stopEvent.value, 0) == WAIT_OBJECT_0)
                    break;
                issueRead();
            }
        } catch (...) {
            // Losing the watcher itself must be observable.  The next refresh
            // can still perform a complete synchronous scan.
            signal();
        }
        drain();
    }
};

DirectoryWatcher::DirectoryWatcher(std::filesystem::path root) : state_(std::make_unique<State>(std::move(root))) {}

DirectoryWatcher::~DirectoryWatcher() = default;

bool DirectoryWatcher::consumeChanged() noexcept {
    return state_ && state_->changed.exchange(false, std::memory_order_acq_rel);
}

} // namespace proto
