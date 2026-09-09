#include "project/FileTransactions.hpp"
#include "assets/AssetIO.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace proto;
namespace fs = std::filesystem;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F&& fn) {
    bool failed{};
    try {
        fn();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "Expected conflict rejection");
}
void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    check(bool(out), "Fixture write failed");
}
std::string read(const fs::path& path) {
    auto bytes = assetBytes(path);
    return {bytes.begin(), bytes.end()};
}
std::map<std::string, FileStamp> snapshot(const fs::path& root) {
    std::map<std::string, FileStamp> result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        auto path = utf8(it->path().lexically_relative(root).generic_wstring());
        if (!projectVisible(path)) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        result.emplace(path, projectStamp(root, path));
    }
    return result;
}
FilePlan movePlan() {
    return {"move UTF-8",
            {{"Assets/Копія", FileKind::Directory, {}, {}},
             {"Assets/Копія/mesh.bin", FileKind::File, {}, "Assets/Джерело/mesh.bin"},
             {"Assets/Копія/readme.txt", FileKind::File, std::string("updated reference"), {}},
             {"Assets/Джерело/mesh.bin", FileKind::Missing, {}, {}},
             {"Assets/Джерело/readme.txt", FileKind::Missing, {}, {}},
             {"Assets/Джерело", FileKind::Missing, {}, {}}},
            {},
            {{"Assets/Джерело", "Assets/Копія"}}};
}
void fixture(const fs::path& root) {
    put(root / L"Assets/Джерело/mesh.bin", std::string(100003, 'x'));
    put(root / L"Assets/Джерело/readme.txt", "old reference");
    put(root / "Code/main.cpp", "// untouched");
}
DWORD runChild(const fs::path& self, const fs::path& root, int step, bool undo, bool recovering = false) {
    std::wstring command = L"\"" + self.wstring() + (recovering ? L"\" --recover \"" : L"\" --crash \"") +
                           root.wstring() + L"\" " + std::to_wstring(step) + (undo ? L" undo" : L" redo");
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    check(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &process),
          "Crash child launch failed");
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 92);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code{};
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(wait == WAIT_OBJECT_0, "Crash child timed out");
    return code;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--recover") {
            const auto step = std::stoul(argv[3]);
            FileTransaction::recover(argv[2], [=](size_t done) {
                if (done == step)
                    ExitProcess(73);
            });
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--crash") {
            auto tx = FileTransaction::prepare(argv[2], movePlan());
            const bool undo = std::wstring_view(argv[4]) == L"undo";
            if (undo)
                tx->redo();
            const auto step = std::stoul(argv[3]);
            tx->afterStep = [=](size_t done) {
                if (done == step)
                    ExitProcess(73);
            };
            if (undo)
                tx->undo();
            else
                tx->redo();
            return 0;
        }
        const auto base = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) /
                          utf8Path("m4-files-" + Uuid::create().string());
        fs::create_directories(base);
        unsigned passed{}, failed{};
        const auto test = [&](const char* name, const auto& fn) {
            try {
                fn();
                ++passed;
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& e) {
                ++failed;
                std::cout << "FAIL " << name << ": " << e.what() << '\n';
            }
        };
        test("multi-file move, disk payloads and stable Undo/Redo", [&] {
            auto root = base / "move";
            fixture(root);
            auto before = snapshot(root);
            auto tx = FileTransaction::prepare(root, movePlan());
            check(snapshot(root) == before, "Prepare modified authored files");
            check(tx->memoryBytes() < 16000 && tx->diskBytes() > 200000, "Binary content leaked into Undo memory");
            tx->redo();
            auto after = snapshot(root);
            check(read(root / L"Assets/Копія/readme.txt") == "updated reference", "Move content mismatch");
            tx->undo();
            check(snapshot(root) == before, "Undo did not restore original tree");
            tx->redo();
            check(snapshot(root) == after, "Redo changed final tree");
        });
        test("external edit and new child reject before Undo mutation", [&] {
            auto root = base / "conflict";
            fixture(root);
            auto tx = FileTransaction::prepare(root, movePlan());
            tx->redo();
            put(root / L"Assets/Копія/mesh.bin", "external edit");
            auto changed = snapshot(root);
            rejects([&] { tx->undo(); });
            check(snapshot(root) == changed, "Undo overwrote external edit");
            put(root / L"Assets/Копія/mesh.bin", std::string(100003, 'x'));
            put(root / L"Assets/Копія/new.txt", "external new");
            changed = snapshot(root);
            rejects([&] { tx->undo(); });
            check(snapshot(root) == changed, "Undo removed external child");
        });
        test("prepared operation rejects stale parsed source", [&] {
            auto root = base / "stale";
            fixture(root);
            auto plan = movePlan();
            plan.guards.push_back({"Code/main.cpp", projectStamp(root, "Code/main.cpp")});
            auto tx = FileTransaction::prepare(root, plan);
            put(root / "Code/main.cpp", "// external");
            auto before = snapshot(root);
            rejects([&] { tx->redo(); });
            check(snapshot(root) == before, "Stale guard operation mutated files");
        });
        test("read-only topology guards remain enforced on Undo and Redo", [&] {
            auto root = base / "lifetime-guards";
            fixture(root);
            auto plan = movePlan();
            plan.guards.push_back({"Code/main.cpp", projectStamp(root, "Code/main.cpp")});
            const auto original = read(root / "Code/main.cpp");
            auto tx = FileTransaction::prepare(root, plan);
            tx->redo();
            put(root / "Code/main.cpp", "// new external reference");
            auto before = snapshot(root);
            rejects([&] { tx->undo(); });
            check(snapshot(root) == before, "Undo ignored read-only topology guard");
            put(root / "Code/main.cpp", original);
            tx->undo();
            put(root / "Code/main.cpp", "// new reference after Undo");
            before = snapshot(root);
            rejects([&] { tx->redo(); });
            check(snapshot(root) == before, "Redo ignored read-only topology guard");
        });
        test("path escapes and Win32 aliases are rejected", [&] {
            auto root = base / "paths";
            fs::create_directories(root);
            for (const auto* bad :
                 {"../outside", "Assets/../../outside", "C:/outside", "/outside", "Assets/file:stream",
                  "Assets/CON.txt", "Assets/trailing.", "Assets/trailing ", ".proto/transactions/bad"})
                rejects([&] { projectPath(root, bad); });
            check(projectStamp(root, "missing").kind == FileKind::Missing, "Missing path stamp mismatch");
        });
        test("fault callback rolls back to original namespace", [&] {
            auto root = base / "fault";
            fixture(root);
            auto before = snapshot(root);
            auto tx = FileTransaction::prepare(root, movePlan());
            tx->afterStep = [](size_t n) {
                if (n == 5)
                    throw std::runtime_error("injected");
            };
            rejects([&] { tx->redo(); });
            check(snapshot(root) == before, "Exception rollback mismatch");
        });
        test("case-only file and folder rename", [&] {
            auto root = base / "case";
            put(root / "Assets/Name/A.txt", "same bytes");
            auto before = snapshot(root);
            FilePlan plan{"case",
                          {{"Assets/name", FileKind::Directory, {}, {}},
                           {"Assets/name/a.txt", FileKind::File, {}, "Assets/Name/A.txt"},
                           {"Assets/Name/A.txt", FileKind::Missing, {}, {}}},
                          {},
                          {{"Assets/Name", "Assets/name"}, {"Assets/Name/A.txt", "Assets/name/a.txt"}}};
            auto tx = FileTransaction::prepare(root, plan);
            tx->redo();
            check(snapshot(root).contains("Assets/name/a.txt"), "Case rename failed");
            tx->undo();
            check(snapshot(root) == before, "Case Undo mismatch");
        });
        test("process termination during redo and undo recovers whole tree", [&] {
            for (bool undo : {false, true})
                for (int step : {1, 3, 5, 7, 9}) {
                    auto root =
                        base / utf8Path(std::string(undo ? "undo-crash-" : "redo-crash-") + std::to_string(step));
                    fixture(root);
                    auto before = snapshot(root), expected = before;
                    if (undo) {
                        auto tx = FileTransaction::prepare(root, movePlan());
                        tx->redo();
                        expected = snapshot(root);
                        tx->undo();
                    }
                    const auto exit = runChild(fs::absolute(argv[0]), root, step, undo);
                    if (exit == 0)
                        continue;
                    check(exit == 73, "Crash child did not reach injected termination");
                    FileTransaction::recover(root);
                    check(snapshot(root) == expected, "Crash recovery tree mismatch");
                }
        });
        test("recovery can itself be interrupted without reversing intent", [&] {
            for (bool undo : {false, true}) {
                auto root = base / (undo ? "double-undo" : "double-redo");
                fixture(root);
                auto expected = snapshot(root);
                if (undo) {
                    auto tx = FileTransaction::prepare(root, movePlan());
                    tx->redo();
                    expected = snapshot(root);
                    tx->undo();
                }
                check(runChild(fs::absolute(argv[0]), root, 5, undo) == 73, "Initial crash injection failed");
                check(runChild(fs::absolute(argv[0]), root, 4, false, true) == 73, "Recovery crash injection failed");
                FileTransaction::recover(root);
                check(snapshot(root) == expected, "Repeated recovery reversed intent");
            }
        });
        test("bad rollback blob prevents initial commit", [&] {
            auto root = base / "bad-blob";
            fixture(root);
            auto before = snapshot(root);
            auto tx = FileTransaction::prepare(root, movePlan());
            for (const auto& file : fs::directory_iterator(tx->journalPath()))
                if (file.path().filename().string().starts_with("before-")) {
                    put(file.path(), "damaged");
                    break;
                }
            rejects([&] { tx->redo(); });
            check(snapshot(root) == before, "Committed with a corrupt rollback blob");
        });
        test("external edit during multi-file apply is preserved", [&] {
            auto root = base / "during";
            put(root / "Assets/A.txt", "old A");
            put(root / "Assets/B.txt", "old B");
            FilePlan plan{"two edits",
                          {{"Assets/A.txt", FileKind::File, std::string("new A"), {}},
                           {"Assets/B.txt", FileKind::File, std::string("new B"), {}}},
                          {},
                          {}};
            auto tx = FileTransaction::prepare(root, plan);
            tx->afterStep = [&](size_t) {
                if (read(root / "Assets/A.txt") == "new A")
                    put(root / "Assets/B.txt", "external B");
            };
            rejects([&] { tx->redo(); });
            check(read(root / "Assets/B.txt") == "external B", "Concurrent external content was overwritten");
        });
        test("recovery rejects an unrelated directory before changing other paths", [&] {
            auto root = base / "foreign-directory";
            fixture(root);
            check(runChild(fs::absolute(argv[0]), root, 5, false) == 73, "Crash injection failed");
            const auto target = projectPath(root, "Assets/Копія/mesh.bin");
            if (fs::exists(target))
                fs::remove(target);
            fs::create_directory(target);
            auto expected = snapshot(root);
            rejects([&] { FileTransaction::recover(root); });
            check(snapshot(root) == expected, "Recovery removed an external directory");
        });
        std::cout << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
