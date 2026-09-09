#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "core/Diagnostics.hpp"
#include "core/Id.hpp"
#include "project/ProjectSession.hpp"
#include "project/FileTransactions.hpp"
#include "project/ProjectWatch.hpp"
#include "scene/SceneIO.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace proto;
namespace fs = std::filesystem;

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("fixture write failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw std::runtime_error("fixture write failed");
}

std::string read(const fs::path& path) {
    const auto bytes = assetBytes(path, 8 * 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool rejects(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool waitChanged(ProjectSession& session, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (session.consumeChanged())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return session.consumeChanged();
}

std::string sidecarFor(const AssetId id) {
    return "{\"format\":\"proto.assetmeta\",\"formatVersion\":1,\"assetId\":" + jsonString(id.string()) +
           ",\"kind\":\"Scene\"}\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path output = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path());
    const fs::path fixture = output / utf8Path("m4-project-Україна spaces-" + Uuid::create().string());
    fs::create_directories(output);
    unsigned passed{};
    unsigned failed{};
    const auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    std::shared_ptr<ProjectSession> session;
    std::string manifest;
    fs::path scenePath;
    AssetId sceneId;
    test("create publishes a complete UTF-8 project", [&] {
        session = ProjectSession::create(fixture, "Проєкт M4");
        check(session->root() == fs::absolute(fixture).lexically_normal(), "root path mismatch");
        check(session->info().name == "Проєкт M4", "project name mismatch");
        check(session->assetWorkspace() && session->assetWorkspace()->root() == session->root(),
              "workspace root mismatch");
        scenePath = session->startupScenePath();
        sceneId = session->info().startupScene;
        check(fs::exists(session->root() / "project.proto.json"), "manifest missing");
        check(fs::exists(scenePath), "startup scene missing");
        check(fs::exists(fs::path(scenePath.wstring() + L".meta")), "startup sidecar missing");
        manifest = read(session->root() / "project.proto.json");
        const auto scene = decodeScene(read(scenePath));
        check(scene.id == sceneId && scene.name == "Main", "startup scene identity mismatch");
        check(scene.entities().size() == 4, "new scene should contain cube, plane, camera and sun");
    });

    // Later cases need the created session and fixture metadata. Preserve the
    // original error instead of dereferencing a null session after setup fails.
    if (failed)
        return 1;

    test("single writer lock and manifest open", [&] {
        check(rejects([&] { (void)ProjectSession::open(fixture / "project.proto.json"); }),
              "second project writer was accepted");
        session.reset();
        auto reopened = ProjectSession::open(fixture / "project.proto.json");
        check(static_cast<bool>(reopened->info().id), "manifest project UUID missing");
        check(reopened->startupScenePath() == scenePath, "manifest open resolved a different startup scene");
        session = std::move(reopened);
    });

    test("refresh rejects corrupt manifest without replacing state", [&] {
        const auto oldInfo = session->info();
        const auto oldScene = session->startupScenePath();
        write(session->root() / "project.proto.json", "{\"format\":");
        check(rejects([&] { session->refresh(); }), "corrupt manifest was accepted");
        check(session->info().id == oldInfo.id && session->info().name == oldInfo.name &&
                  session->startupScenePath() == oldScene,
              "failed refresh replaced the caller state");
        write(session->root() / "project.proto.json", manifest);
        session->refresh();
    });

    test("scene sidecar mismatch and duplicate UUIDs reject", [&] {
        session.reset();
        const auto sidecar = fs::path(scenePath.wstring() + L".meta");
        write(sidecar, sidecarFor(AssetId::create()));
        check(rejects([&] { (void)ProjectSession::open(fixture); }), "sidecar UUID mismatch was accepted");
        write(sidecar, sidecarFor(sceneId));
        const auto copy = fixture / "Scenes" / "Duplicate.scene.json";
        fs::copy_file(scenePath, copy);
        fs::copy_file(sidecar, fs::path(copy.wstring() + L".meta"));
        check(rejects([&] { (void)ProjectSession::open(fixture); }), "duplicate scene UUID was accepted");
        fs::remove(copy);
        fs::remove(fs::path(copy.wstring() + L".meta"));
        session = ProjectSession::open(fixture);
    });

    test("refresh preserves live transaction snapshots and Undo Redo", [&] {
        FilePlan plan{"Live history", {{"Code/history.txt", FileKind::File, std::string("snapshot"), {}}}};
        auto transaction = FileTransaction::prepare(fixture, plan);
        transaction->redo();
        session->refresh();
        transaction->undo();
        check(!fs::exists(fixture / "Code/history.txt"), "refresh destroyed Undo snapshots");
        session->refresh();
        transaction->redo();
        check(read(fixture / "Code/history.txt") == "snapshot", "refresh destroyed Redo snapshots");
    });
    test("watcher reports authored changes and stays quiet for service files", [&] {
        while (session->consumeChanged()) {
        }
        write(session->root() / "Code" / "Зміна.cpp", "// authored\n");
        check(waitChanged(*session), "authored file notification missing");
        while (session->consumeChanged()) {
        }
        // The completion may arrive a little after the first coalesced bit;
        // establish a quiet interval before checking service-file filtering.
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            while (session->consumeChanged()) {
            }
        }
        write(session->root() / ".proto" / "cache.tmp-ignored", "service\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(!session->consumeChanged(), "service file notification was not filtered");
    });

    test("watcher drains pending I/O during immediate and active-write shutdown", [&] {
        const auto root = session->root();
        for (int i = 0; i < 100; ++i) {
            DirectoryWatcher watcher(root);
        }
        for (int i = 0; i < 50; ++i) {
            DirectoryWatcher watcher(root);
            write(root / "Code/watch-stress.txt", std::to_string(i));
        }
        DirectoryWatcher watcher(root);
        write(root / "Code/watch-immediate.txt", "immediate");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool changed{};
        while (!changed && std::chrono::steady_clock::now() < deadline) {
            changed = watcher.consumeChanged();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(changed, "watcher constructor returned before it was armed");
    });
    test("open recreates an omitted derived service directory", [&] {
        session.reset();
        const auto control = projectPath(fixture, ".proto", true);
        fs::remove_all(control);
        session = ProjectSession::open(fixture);
        check(fs::is_directory(control) && session->info().startupScene == sceneId,
              "Project could not reopen without its derived cache directory");
    });
    test("opening an ordinary folder does not create service files", [&] {
        const auto ordinary = fixture / "Code/Ordinary";
        fs::create_directory(ordinary);
        check(rejects([&] { (void)ProjectSession::open(ordinary); }), "Ordinary folder was accepted as project");
        check(!fs::exists(ordinary / ".proto"), "Failed open wrote service files into an ordinary folder");
    });
    session.reset();
    std::error_code removeError;
    (void)projectRelative(output, fixture);
    fs::remove_all(fixture, removeError);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
