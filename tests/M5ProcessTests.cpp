#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "assets/AssetIO.hpp"
#include "project/ManagedProcess.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace proto;
namespace fs = std::filesystem;

namespace {

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void writeHandle(HANDLE handle, std::string_view bytes) {
    size_t offset{};
    while (offset < bytes.size()) {
        DWORD written{};
        const auto amount = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 64 * 1024));
        check(WriteFile(handle, bytes.data() + offset, amount, &written, nullptr) && written == amount,
              "child output write failed");
        offset += written;
    }
}

void childMode(int argc, wchar_t** argv) {
    if (argc < 2)
        return;
    const std::wstring_view mode(argv[1]);
    if (mode == L"--emit") {
        auto* output = GetStdHandle(STD_OUTPUT_HANDLE);
        for (int i = 2; i < argc; ++i) {
            writeHandle(output, "arg=" + utf8(argv[i]) + "\n");
        }
        return;
    }
    if (mode == L"--burst") {
        auto* error = GetStdHandle(STD_ERROR_HANDLE);
        writeHandle(error, std::string(2 * 1024 * 1024, 'e'));
        return;
    }
    if (mode == L"--flood") {
        const std::string bytes(64 * 1024, 'f');
        for (;;) {
            writeHandle(GetStdHandle(STD_OUTPUT_HANDLE), bytes);
            writeHandle(GetStdHandle(STD_ERROR_HANDLE), bytes);
        }
    }
    if (mode == L"--hang") {
        for (;;)
            Sleep(1000);
    }
    if (mode == L"--hang-grandchild") {
        check(argc >= 3, "grandchild pid path missing");
        std::ofstream pidFile(fs::path(argv[2]), std::ios::trunc);
        check(bool(pidFile), "grandchild pid file could not be opened");
        pidFile << GetCurrentProcessId();
        pidFile.flush();
        for (;;)
            Sleep(1000);
    }
    if (mode == L"--hang-tree" || mode == L"--exit-tree") {
        check(argc >= 3, "tree pid path missing");
        wchar_t modulePath[32768]{};
        const auto length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        check(length && length < std::size(modulePath), "tree helper module path failed");
        std::wstring command =
            L"\"" + std::wstring(modulePath, length) + L"\" --hang-grandchild \"" + std::wstring(argv[2]) + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        check(CreateProcessW(modulePath, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup, &process),
              "grandchild launch failed");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (mode == L"--exit-tree") {
            for (int i = 0; i != 400 && !fs::exists(fs::path(argv[2])); ++i)
                Sleep(5);
            return;
        }
        for (;;)
            Sleep(1000);
    }
    if (mode == L"--crash")
        ExitProcess(73);
    if (mode == L"--wait-event") {
        check(argc >= 3, "event name missing");
        const auto event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        check(event != nullptr, "child could not open stop event");
        const auto result = WaitForSingleObject(event, 5000);
        CloseHandle(event);
        check(result == WAIT_OBJECT_0, "child stop event did not signal");
        return;
    }
}

struct Completed {
    uint32_t code{};
    std::string out;
    std::string err;
    bool outTruncated{};
    bool errTruncated{};
};

Completed run(const fs::path& self, std::vector<std::string> arguments, size_t limit = 4 * 1024 * 1024) {
    ManagedProcess process;
    ProcessSpec spec;
    spec.executable = self;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = self.parent_path();
    spec.stdoutLimit = limit;
    spec.stderrLimit = limit;
    process.start(spec);
    Completed result;
    for (;;) {
        const auto poll = process.poll();
        result.out += process.takeStdout();
        result.err += process.takeStderr();
        result.outTruncated = result.outTruncated || poll.stdoutTruncated;
        result.errTruncated = result.errTruncated || poll.stderrTruncated;
        if (!poll.running) {
            result.code = poll.exitCode.value_or(1);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]).starts_with(L"--")) {
        try {
            childMode(argc, argv);
            return 0;
        } catch (const std::exception& error) {
            writeHandle(GetStdHandle(STD_ERROR_HANDLE), error.what());
            return 91;
        }
    }
    try {
        const auto self = fs::absolute(argv[0]).lexically_normal();
        unsigned passed{}, failed{};
        const auto test = [&](const char* name, const auto& action) {
            try {
                action();
                ++passed;
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            }
        };

        test("Unicode, spaces, quotes and empty arguments survive CreateProcessW", [&] {
            const auto result = run(self, {"--emit", "Привіт світ", "quote \" and slash\\", ""});
            check(result.code == 0, "argument child failed");
            check(result.out.find("arg=Привіт світ\n") != std::string::npos, "Unicode argument was changed");
            check(result.out.find("arg=quote \" and slash\\\n") != std::string::npos, "quoted argument was changed");
            check(result.out.find("arg=\n") != std::string::npos, "empty argument was changed");
        });

        test("stdout and stderr remain separate while a burst is drained", [&] {
            const auto result = run(self, {"--burst"}, 64 * 1024);
            check(result.code == 0, "burst child failed");
            check(result.out.empty(), "stderr burst leaked into stdout");
            check(result.err.size() == 64 * 1024 && result.errTruncated, "stderr bound was not enforced");
        });

        test("continuous output cannot starve bounded cancellation", [&] {
            ManagedProcess process;
            process.start(ProcessSpec{self, {"--flood"}, self.parent_path()});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto started = std::chrono::steady_clock::now();
            process.forceStop(std::chrono::milliseconds(1500));
            const auto elapsed = std::chrono::steady_clock::now() - started;
            check(!process.running(), "flood child survived forceStop");
            check(elapsed < std::chrono::seconds(2), "continuous output delayed forceStop");
        });

        test("owned named stop event signals a cooperative child", [&] {
            ManagedProcess process;
            const auto eventName = process.prepareStopEvent();
            ProcessSpec spec{self, {"--wait-event", utf8(eventName)}, self.parent_path()};
            process.start(spec);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            process.signalStop();
            for (;;) {
                const auto poll = process.poll();
                (void)process.takeStdout();
                (void)process.takeStderr();
                if (!poll.running) {
                    check(poll.exitCode.value_or(1) == 0, "cooperative child failed");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        test("crashed and hung children are bounded", [&] {
            const auto crash = run(self, {"--crash"});
            check(crash.code == 73, "crash exit code was lost");
            ManagedProcess process;
            process.start(ProcessSpec{self, {"--hang"}, self.parent_path()});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            process.forceStop(std::chrono::milliseconds(1500));
            check(!process.running(), "forceStop did not end owned child");
        });

        test("forceStop terminates the complete owned process tree", [&] {
            const auto pidFile =
                fs::temp_directory_path() / utf8Path("proto-m5-tree-" + Uuid::create().string() + ".txt");
            ManagedProcess process;
            process.start(ProcessSpec{self, {"--hang-tree", utf8(pidFile.wstring())}, self.parent_path()});
            uint32_t grandchild{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline && !grandchild) {
                std::ifstream input(pidFile);
                if (input)
                    input >> grandchild;
                if (!grandchild)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(grandchild != 0, "grandchild did not start");
            const auto child = OpenProcess(SYNCHRONIZE, FALSE, grandchild);
            check(child != nullptr, "grandchild process handle could not be opened");
            process.forceStop(std::chrono::milliseconds(0));
            if (!process.poll().running)
                check(WaitForSingleObject(child, 0) == WAIT_OBJECT_0, "zero-wait cleanup released a live descendant");
            process.forceStop(std::chrono::milliseconds(1500));
            check(WaitForSingleObject(child, 0) == WAIT_OBJECT_0, "owned grandchild survived forceStop");
            CloseHandle(child);
            std::error_code ignored;
            fs::remove(pidFile, ignored);
        });

        test("root exit closes owned descendants before process completion", [&] {
            const auto pidFile =
                fs::temp_directory_path() / utf8Path("proto-m5-exit-tree-" + Uuid::create().string() + ".txt");
            ManagedProcess process;
            process.start(ProcessSpec{self, {"--exit-tree", utf8(pidFile.wstring())}, self.parent_path()});
            uint32_t grandchild{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline && !grandchild) {
                std::ifstream input(pidFile);
                if (input)
                    input >> grandchild;
                if (!grandchild)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(grandchild != 0, "exit-tree grandchild did not start");
            const auto child = OpenProcess(SYNCHRONIZE, FALSE, grandchild);
            check(child != nullptr, "exit-tree grandchild handle could not be opened");
            for (;;) {
                const auto poll = process.poll();
                (void)process.takeStdout();
                (void)process.takeStderr();
                if (!poll.running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(WaitForSingleObject(child, 0) == WAIT_OBJECT_0, "owned descendant survived root exit");
            CloseHandle(child);
            std::error_code ignored;
            fs::remove(pidFile, ignored);
        });

        test("failure before job assignment terminates the suspended root", [&] {
            ManagedProcess process;
            uint32_t pid{};
            bool rejected{};
            ProcessSpec spec{self, {"--hang"}, self.parent_path()};
            spec.diagnosticBeforeJobAssignment = [&](uint32_t created) {
                pid = created;
                throw std::runtime_error("injected assignment failure");
            };
            try {
                process.start(spec);
            } catch (const std::exception&) {
                rejected = true;
            }
            check(rejected && pid != 0, "assignment failure was not injected");
            const auto child = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (child) {
                const auto exited = WaitForSingleObject(child, 1000);
                CloseHandle(child);
                check(exited == WAIT_OBJECT_0, "suspended unassigned child survived");
            }
        });
        std::cout << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
