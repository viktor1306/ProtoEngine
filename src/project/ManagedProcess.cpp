#include "project/ManagedProcess.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace proto {
namespace {

class WinHandle {
  public:
    WinHandle() = default;
    explicit WinHandle(HANDLE value) : value_(value) {}
    ~WinHandle() { reset(); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_{};
};

[[noreturn]] void winError(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (Windows " + std::to_string(GetLastError()) + ")");
}

std::wstring wide(std::string_view value) {
    if (value.size() > static_cast<size_t>(INT_MAX))
        throw std::runtime_error("UTF-8 process argument is too long");
    if (value.find('\0') != std::string_view::npos)
        throw std::runtime_error("Process argument contains NUL");
    if (value.empty())
        return {};
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Invalid UTF-8 process argument");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                             count))
        winError("MultiByteToWideChar");
    return result;
}

std::wstring quoteArg(std::wstring_view argument) {
    // CommandLineToArgvW-compatible quoting.  lpApplicationName is also set
    // to the exact executable path, so this string is never shell parsed.
    std::wstring result;
    result.reserve(argument.size() + 2);
    result.push_back(L'"');
    size_t slashes{};
    for (const auto c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(slashes, L'\\');
            result.push_back(c);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring commandLine(const std::wstring& executable, const std::vector<std::string>& arguments) {
    std::wstring result = quoteArg(executable);
    for (const auto& argument : arguments) {
        result.push_back(L' ');
        result += quoteArg(wide(argument));
    }
    return result;
}

bool compilerEnvironmentName(std::wstring_view name) {
    constexpr std::array<std::wstring_view, 10> names = {
        L"CXXFLAGS", L"CPPFLAGS",           L"LDFLAGS",      L"CXX",           L"CC",
        L"CPATH",    L"CPLUS_INCLUDE_PATH", L"LIBRARY_PATH", L"COMPILER_PATH", L"GCC_EXEC_PREFIX"};
    return std::any_of(names.begin(), names.end(), [&](const auto candidate) {
        return name.size() == candidate.size() &&
               std::equal(name.begin(), name.end(), candidate.begin(), [](wchar_t a, wchar_t b) {
                   if (a >= L'a' && a <= L'z')
                       a = static_cast<wchar_t>(a - L'a' + L'A');
                   return a == b;
               });
    });
}

std::vector<wchar_t> cleanCompilerEnvironment(const std::filesystem::path& toolchainDirectory) {
    const auto source = GetEnvironmentStringsW();
    if (!source)
        winError("GetEnvironmentStringsW");
    struct EnvironmentGuard {
        LPWCH value;
        ~EnvironmentGuard() { FreeEnvironmentStringsW(value); }
    } guard{source};

    std::vector<std::wstring> entries;
    bool hasPath{};
    for (auto* entry = source; *entry; entry += std::wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        const auto equals = text.find(L'=');
        if (equals != std::wstring_view::npos && compilerEnvironmentName(text.substr(0, equals)))
            continue;
        if (!toolchainDirectory.empty() && equals == 4 &&
            CompareStringOrdinal(text.data(), 4, L"PATH", 4, TRUE) == CSTR_EQUAL) {
            entries.push_back(L"PATH=" + toolchainDirectory.wstring() + L";" + std::wstring(text.substr(equals + 1)));
            hasPath = true;
        } else {
            entries.emplace_back(text);
        }
    }
    if (!toolchainDirectory.empty() && !hasPath)
        entries.push_back(L"PATH=" + toolchainDirectory.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    if (result.empty()) {
        result.push_back(L'\0');
        result.push_back(L'\0');
    } else {
        result.push_back(L'\0');
    }
    return result;
}

void appendBounded(std::string& output, size_t& total, std::string_view bytes, size_t limit, bool& truncated) {
    if (total >= limit) {
        truncated = truncated || !bytes.empty();
        total += bytes.size();
        return;
    }
    const auto amount = std::min(limit - total, bytes.size());
    output.append(bytes.data(), amount);
    total += bytes.size();
    if (amount != bytes.size())
        truncated = true;
}

std::wstring uniqueEventName(std::wstring_view prefix) {
    static volatile LONG64 sequence{};
    if (prefix.empty())
        prefix = L"ProtoPlayer.Stop";
    if (prefix.size() > 200)
        throw std::runtime_error("Stop-event name prefix is too long");
    std::wstring base(prefix);
    if (base.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("Stop-event name contains NUL");
    const auto value = InterlockedIncrement64(&sequence);
    return L"Local\\" + base + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(value) + L"." +
           std::to_wstring(GetTickCount64());
}

void updateStats(HANDLE process, ProcessStats& stats) noexcept {
    if (!process)
        return;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        stats.cpuSeconds = static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
    }

    // K32GetProcessMemoryInfo lives in kernel32 on modern Windows.  Resolve
    // it dynamically so the process helper does not impose a psapi link on
    // every editor target.
    using MemoryInfoFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    const auto module = GetModuleHandleW(L"kernel32.dll");
    MemoryInfoFn function{};
    if (module) {
        const auto raw = GetProcAddress(module, "K32GetProcessMemoryInfo");
        static_assert(sizeof(raw) == sizeof(function));
        std::memcpy(&function, &raw, sizeof(function));
    }
    if (function) {
        PROCESS_MEMORY_COUNTERS counters{};
        if (function(process, &counters, sizeof(counters)))
            stats.workingSetBytes = static_cast<uint64_t>(counters.WorkingSetSize);
    }
}

bool waitJobEmpty(HANDLE job, std::chrono::milliseconds timeout) noexcept {
    if (!job)
        return true;
    const auto bounded = std::max(timeout, std::chrono::milliseconds(0));
    const auto deadline = std::chrono::steady_clock::now() + bounded;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting),
                                      nullptr) &&
            accounting.ActiveProcesses == 0)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        Sleep(1);
    }
}

bool terminateJob(HANDLE job, std::chrono::milliseconds timeout, std::vector<WinHandle>& handles) noexcept {
    if (!job)
        return true;
    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
    try {
        size_t capacity = 32;
        while (handles.empty()) {
            std::vector<std::byte> bytes(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + capacity * sizeof(ULONG_PTR));
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(bytes.data());
            if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(bytes.size()),
                                          nullptr)) {
                for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
                    WinHandle process(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(list->ProcessIdList[i])));
                    if (process)
                        handles.push_back(std::move(process));
                }
                break;
            }
            if (GetLastError() != ERROR_MORE_DATA || capacity >= 65536)
                break;
            capacity = std::max(capacity * 2, size_t(list->NumberOfAssignedProcesses));
        }
    } catch (...) {
    }
    (void)TerminateJobObject(job, ERROR_CANCELLED);
    // ActiveProcesses can reach zero before the process objects are signalled.
    // Retain handles collected before termination and wait for actual exit too.
    while (!handles.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero()));
        if (WaitForSingleObject(handles.back().get(), static_cast<DWORD>(left.count())) != WAIT_OBJECT_0)
            return false;
        handles.pop_back();
    }
    return waitJobEmpty(
        job, std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero())));
}

} // namespace

struct ManagedProcess::State {
    WinHandle process;
    WinHandle thread;
    WinHandle job;
    WinHandle stdoutRead;
    WinHandle stderrRead;
    WinHandle stopEvent;
    // Retain process objects across bounded cleanup attempts; job accounting
    // can become empty before all captured process handles signal completion.
    std::vector<WinHandle> pendingExitHandles;
    std::wstring stopName;
    std::string stdoutBytes;
    std::string stderrBytes;
    size_t stdoutTotal{};
    size_t stderrTotal{};
    size_t stdoutLimit{4 * 1024 * 1024};
    size_t stderrLimit{4 * 1024 * 1024};
    bool stdoutTruncated{};
    bool stderrTruncated{};
    bool started{};
    bool running{};
    uint32_t exitCode{};
    uint32_t processId{};
    ProcessStats stats;

    ~State() {
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE terminates every descendant when
        // this owner goes away.  Explicit termination also covers a process
        // whose job limits were not applied because startup failed halfway.
        if (job) {
            if (running)
                TerminateJobObject(job.get(), ERROR_CANCELLED);
            job.reset();
        }
    }

    void clearProcess() {
        if (running && job) {
            TerminateJobObject(job.get(), ERROR_CANCELLED);
            if (process)
                (void)WaitForSingleObject(process.get(), 2000);
        }
        process.reset();
        thread.reset();
        job.reset();
        stdoutRead.reset();
        stderrRead.reset();
        started = false;
        running = false;
        exitCode = 0;
        processId = 0;
        stats = {};
    }
};

ManagedProcess::ManagedProcess() : state_(new State) {}

ManagedProcess::~ManagedProcess() {
    if (state_) {
        state_->stopEvent.reset();
        delete state_;
        state_ = nullptr;
    }
}

ManagedProcess::ManagedProcess(ManagedProcess&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ManagedProcess& ManagedProcess::operator=(ManagedProcess&& other) noexcept {
    if (this != &other) {
        if (state_)
            delete state_;
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}

std::wstring ManagedProcess::prepareStopEvent(std::wstring_view prefix) {
    if (!state_)
        throw std::runtime_error("ManagedProcess is moved-from");
    state_->stopEvent.reset();
    state_->stopName.clear();
    const auto name = uniqueEventName(prefix);
    state_->stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
    if (!state_->stopEvent)
        winError("CreateEventW");
    state_->stopName = name;
    return state_->stopName;
}

void ManagedProcess::resetStop() {
    if (!state_ || !state_->stopEvent)
        return;
    if (!ResetEvent(state_->stopEvent.get()))
        winError("ResetEvent");
}

void ManagedProcess::signalStop() {
    if (!state_ || !state_->stopEvent)
        throw std::runtime_error("No stop event was prepared");
    if (!SetEvent(state_->stopEvent.get()))
        winError("SetEvent");
}

const std::wstring& ManagedProcess::stopEventName() const noexcept {
    static const std::wstring empty;
    return state_ ? state_->stopName : empty;
}

void ManagedProcess::start(const ProcessSpec& spec) {
    if (!state_)
        throw std::runtime_error("ManagedProcess is moved-from");
    if (spec.executable.empty())
        throw std::runtime_error("Process executable is empty");
    if (state_->running)
        throw std::runtime_error("ManagedProcess is already running");
    state_->clearProcess();
    state_->stdoutBytes.clear();
    state_->stderrBytes.clear();
    state_->stdoutTotal = 0;
    state_->stderrTotal = 0;
    state_->stdoutTruncated = false;
    state_->stderrTruncated = false;
    if (state_->stopEvent && !ResetEvent(state_->stopEvent.get()))
        winError("ResetEvent");
    state_->stdoutLimit = spec.stdoutLimit;
    state_->stderrLimit = spec.stderrLimit;

    const auto executable = std::filesystem::absolute(spec.executable).lexically_normal();
    if (!std::filesystem::exists(executable) || !std::filesystem::is_regular_file(executable))
        throw std::runtime_error("Process executable does not exist: " + executable.string());
    const auto executableText = executable.wstring();
    const auto workingDirectory = spec.workingDirectory.empty()
                                      ? std::wstring{}
                                      : std::filesystem::absolute(spec.workingDirectory).lexically_normal().wstring();

    SECURITY_ATTRIBUTES inherited{};
    inherited.nLength = sizeof(inherited);
    inherited.bInheritHandle = TRUE;
    HANDLE stdoutReadRaw{}, stdoutWriteRaw{}, stderrReadRaw{}, stderrWriteRaw{};
    if (!CreatePipe(&stdoutReadRaw, &stdoutWriteRaw, &inherited, 0))
        winError("CreatePipe(stdout)");
    WinHandle stdoutRead(stdoutReadRaw), stdoutWrite(stdoutWriteRaw), stdinRead;
    if (!SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0))
        winError("SetHandleInformation(stdout)");
    if (!CreatePipe(&stderrReadRaw, &stderrWriteRaw, &inherited, 0))
        winError("CreatePipe(stderr)");
    WinHandle stderrRead(stderrReadRaw), stderrWrite(stderrWriteRaw);
    if (!SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0))
        winError("SetHandleInformation(stderr)");
    stdinRead.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!stdinRead)
        winError("CreateFileW(NUL)");

    SIZE_T attributeBytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (!attributeBytes)
        winError("InitializeProcThreadAttributeList(size)");
    std::vector<std::byte> attributes(attributeBytes);
    auto* startupAttributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startupAttributes, 1, 0, &attributeBytes))
        winError("InitializeProcThreadAttributeList");
    const HANDLE inheritedHandles[] = {stdinRead.get(), stdoutWrite.get(), stderrWrite.get()};
    if (!UpdateProcThreadAttribute(startupAttributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   const_cast<HANDLE*>(inheritedHandles), sizeof(inheritedHandles), nullptr, nullptr))
        winError("UpdateProcThreadAttribute");

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdinRead.get();
    startup.StartupInfo.hStdOutput = stdoutWrite.get();
    startup.StartupInfo.hStdError = stderrWrite.get();
    startup.lpAttributeList = startupAttributes;
    PROCESS_INFORMATION processInfo{};
    auto command = commandLine(executableText, spec.arguments);
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    auto environment = spec.clearToolchainEnvironment ? cleanCompilerEnvironment(spec.toolchainDirectory)
                                                     : std::vector<wchar_t>{};
    const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    const BOOL created = CreateProcessW(executableText.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE, flags,
                                        environment.empty() ? nullptr : environment.data(),
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup.StartupInfo, &processInfo);
    const auto createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startupAttributes);
    if (!created) {
        SetLastError(createError);
        winError("CreateProcessW");
    }
    WinHandle process(processInfo.hProcess), thread(processInfo.hThread);
    // The parent must close its copies before the child is resumed so pipe
    // reads can observe EOF once the process exits.
    stdoutWrite.reset();
    stderrWrite.reset();
    stdinRead.reset();

    WinHandle job;
    try {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job)
            winError("CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            winError("SetInformationJobObject");
        if (spec.diagnosticBeforeJobAssignment)
            spec.diagnosticBeforeJobAssignment(processInfo.dwProcessId);
        if (!AssignProcessToJobObject(job.get(), process.get()))
            winError("AssignProcessToJobObject");
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
            winError("ResumeThread");
    } catch (...) {
        // A failure before AssignProcessToJobObject can leave an empty job
        // while the suspended root remains outside it.  Terminate the root in
        // every startup-failure path; the job termination covers descendants
        // when assignment did succeed.
        TerminateProcess(process.get(), ERROR_CANCELLED);
        if (job)
            TerminateJobObject(job.get(), ERROR_CANCELLED);
        (void)WaitForSingleObject(process.get(), 2000);
        throw;
    }

    state_->process = std::move(process);
    state_->thread = std::move(thread);
    state_->job = std::move(job);
    state_->stdoutRead = std::move(stdoutRead);
    state_->stderrRead = std::move(stderrRead);
    state_->started = true;
    state_->running = true;
    state_->processId = processInfo.dwProcessId;
}

namespace {
size_t drainPipe(WinHandle& read, std::string& destination, size_t& capturedTotal, size_t limit, bool& truncated) {
    if (!read)
        return 0;
    constexpr size_t maxBytesPerPoll = 256 * 1024;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    size_t readTotal{};
    for (;;) {
        if (readTotal >= maxBytesPerPoll || std::chrono::steady_clock::now() >= deadline)
            break;
        DWORD available{};
        if (!PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                read.reset();
                break;
            }
            // A pipe can close between PeekNamedPipe and ReadFile.  Treat
            // that race as EOF; other errors are surfaced to the caller.
            if (error == ERROR_INVALID_HANDLE) {
                read.reset();
                break;
            }
            winError("PeekNamedPipe");
        }
        if (!available)
            break;
        const DWORD amount =
            std::min<DWORD>(available, static_cast<DWORD>(std::min<size_t>(64 * 1024, maxBytesPerPoll - readTotal)));
        std::array<char, 64 * 1024> buffer{};
        DWORD readBytes{};
        if (!ReadFile(read.get(), buffer.data(), amount, &readBytes, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                read.reset();
                break;
            }
            winError("ReadFile(pipe)");
        }
        if (!readBytes)
            break;
        // The stream counters remain cumulative even when the caller takes
        // and clears the current chunk.  This keeps ProcessSpec limits global
        // across a long-running child rather than per poll interval.
        appendBounded(destination, capturedTotal, std::string_view(buffer.data(), readBytes), limit, truncated);
        readTotal += readBytes;
        if (readBytes < amount)
            break;
    }
    return readTotal;
}
} // namespace

ProcessPoll ManagedProcess::poll() {
    if (!state_)
        throw std::runtime_error("ManagedProcess is moved-from");
    const auto stdoutBytes = drainPipe(state_->stdoutRead, state_->stdoutBytes, state_->stdoutTotal,
                                       state_->stdoutLimit, state_->stdoutTruncated);
    const auto stderrBytes = drainPipe(state_->stderrRead, state_->stderrBytes, state_->stderrTotal,
                                       state_->stderrLimit, state_->stderrTruncated);
    if (state_->running) {
        const auto wait = WaitForSingleObject(state_->process.get(), 0);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode{};
            if (!GetExitCodeProcess(state_->process.get(), &exitCode))
                winError("GetExitCodeProcess");
            state_->exitCode = exitCode;
            // A child can leave descendants behind after its own exit.  The
            // job is still owned by this object, so terminate the remaining
            // tree as soon as root completion is observed rather than
            // waiting for a later destructor or restart.
            state_->running =
                !terminateJob(state_->job.get(), std::chrono::milliseconds(0), state_->pendingExitHandles);
        } else if (wait != WAIT_TIMEOUT) {
            winError("WaitForSingleObject");
        }
    }
    updateStats(state_->process.get(), state_->stats);
    return {state_->running,
            state_->running ? std::optional<uint32_t>{} : std::optional<uint32_t>{state_->exitCode},
            stdoutBytes,
            stderrBytes,
            state_->stdoutTruncated,
            state_->stderrTruncated};
}

bool ManagedProcess::running() const noexcept {
    // Keep the logical state until poll() observes exit and drains the final
    // pipe batch.  Callers commonly use running() to decide whether to call
    // poll(); probing the kernel here would make a naturally exited process
    // skip that finalization path entirely.
    return state_ && state_->started && state_->running;
}

std::optional<uint32_t> ManagedProcess::exitCode() const noexcept {
    if (!state_ || !state_->started || state_->running)
        return {};
    return state_->exitCode;
}

std::string ManagedProcess::takeStdout() {
    (void)poll();
    return std::exchange(state_->stdoutBytes, {});
}

std::string ManagedProcess::takeStderr() {
    (void)poll();
    return std::exchange(state_->stderrBytes, {});
}

bool ManagedProcess::stdoutTruncated() const noexcept {
    return state_ && state_->stdoutTruncated;
}

bool ManagedProcess::stderrTruncated() const noexcept {
    return state_ && state_->stderrTruncated;
}

void ManagedProcess::forceStop(std::chrono::milliseconds wait) {
    if (!state_ || !state_->started)
        return;
    if (state_->running)
        (void)poll();
    const auto bounded = std::max(wait, std::chrono::milliseconds(0));
    const auto deadline = std::chrono::steady_clock::now() + bounded;
    if (state_->job) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero()));
        (void)terminateJob(state_->job.get(), remaining, state_->pendingExitHandles);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero()));
    const auto result =
        WaitForSingleObject(state_->process.get(), static_cast<DWORD>(std::min<int64_t>(remaining.count(), MAXDWORD)));
    if (result == WAIT_FAILED)
        winError("WaitForSingleObject");
    (void)poll();
    if (state_->running) {
        // TerminateJobObject is asynchronous.  Marking a still-live handle
        // as running keeps the destructor's kill-on-close safety net active.
        return;
    }
}

ProcessStats ManagedProcess::stats() const noexcept {
    if (!state_)
        return {};
    updateStats(state_->process.get(), state_->stats);
    return state_->stats;
}

uint32_t ManagedProcess::processId() const noexcept {
    return state_ ? state_->processId : 0;
}

} // namespace proto
