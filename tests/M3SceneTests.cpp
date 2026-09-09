#include "editor/SceneDocument.hpp"
#include <windows.h>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>

using namespace proto;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    bool failed{};
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "Expected rejection");
}
EntityRecord object(std::string name = "Об’єкт") {
    EntityRecord result;
    result.id = EntityId::create();
    result.name = std::move(name);
    return result;
}
std::string replaceOnce(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "Fixture token missing");
    text.replace(at, from.size(), to);
    return text;
}
void put(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    check(bool(stream), "Fixture write failed");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto root = (argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("test-results")) / L"M3 сцена";
    std::filesystem::create_directories(root);
    unsigned passed{}, failed{};
    auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("light components have dense lifecycle and reject mixed types", [&] {
        Scene scene;
        auto directional = object("Sun");
        directional.mesh = MeshRenderer{};
        directional.directionalLight = DirectionalLight{};
        const auto sun = scene.create(directional);
        check(scene.directionalLights().size() == 1 && scene.pointLights().empty(), "Directional pool missing item");
        check(scene.directionalLight(sun) && scene.mesh(sun), "Mesh plus directional light was not retained");
        check(scene.record(sun) == directional, "Directional record roundtrip failed");

        auto point = scene.record(sun);
        point.directionalLight.reset();
        point.pointLight = PointLight{};
        point.pointLight->radius = 12.0f;
        scene.edit(point);
        check(!scene.directionalLight(sun) && scene.pointLight(sun) && scene.pointLights().size() == 1,
              "Edit did not swap dense light pools");
        auto mixed = point;
        mixed.directionalLight = DirectionalLight{};
        rejects([&] { scene.edit(mixed); });
        check(scene.pointLight(sun) && !scene.directionalLight(sun), "Rejected mixed edit mutated scene");
        scene.eraseSubtree(sun);
        check(scene.pointLights().empty() && scene.directionalLights().empty(), "Erase left a light pool item");
        scene.restore({point});
        const auto restored = scene.find(point.id);
        check(restored && scene.pointLight(restored) && scene.record(restored) == point, "Restore lost point light");
    });

    test("parent transform and enable state apply to lights", [&] {
        Scene scene;
        auto parent = object("Parent");
        parent.transform.position = {2.0f, 1.0f, -3.0f};
        const auto parentHandle = scene.create(parent);
        auto child = object("Point");
        child.parent = parent.id;
        child.transform.position = {1.0f, 0.5f, -2.0f};
        child.pointLight = PointLight{};
        const auto childHandle = scene.create(child);
        scene.update();
        check(scene.transform(childHandle).visible, "Enabled light should be visible");
        const auto expected = matrix(parent.transform) * matrix(child.transform);
        check(glm::all(glm::lessThan(glm::abs(glm::vec4(scene.transform(childHandle).world[3] - expected[3])),
                                     glm::vec4(0.0001f))),
              "Light world transform was not parented");
        parent.enabled = false;
        scene.edit(parent);
        check(!scene.transform(childHandle).visible, "Disabled parent did not disable light");
        (void)parentHandle;
    });

    test("light property gesture creates one Undo command", [&] {
        SceneDocument document;
        auto light = object("Point");
        light.pointLight = PointLight{};
        const auto id = document.create(light);
        const auto history = document.undoCount();
        document.beginGesture(id);
        for (int i = 1; i <= 8; ++i) {
            auto next = document.scene.record(document.scene.find(id));
            next.pointLight->intensity = float(i) * 10.0f;
            document.preview(next);
        }
        document.endGesture();
        check(document.undoCount() == history + 1, "Light gesture created multiple commands");
        check(document.scene.pointLight(document.scene.find(id))->intensity == 80.0f, "Gesture final value missing");
        document.undo();
        check(document.scene.pointLight(document.scene.find(id))->intensity == PointLight{}.intensity,
              "Light gesture Undo failed");
        document.redo();
        check(document.scene.pointLight(document.scene.find(id))->intensity == 80.0f, "Light gesture Redo failed");
    });

    test("lighting settings validate atomically and support Undo/Redo", [&] {
        SceneDocument document;
        const auto original = document.scene.lighting;
        const auto history = document.undoCount();
        auto changed = original;
        changed.shadowResolution = 1024;
        changed.shadowPoolMiB = 64;
        changed.ambient = 0.2f;
        document.lighting(changed);
        check(document.scene.lighting == changed && document.undoCount() == history + 1, "Settings command missing");
        document.undo();
        check(document.scene.lighting == original, "Settings Undo failed");
        document.redo();
        check(document.scene.lighting == changed, "Settings Redo failed");
        const auto count = document.undoCount();
        auto invalid = changed;
        invalid.shadowResolution = 300;
        rejects([&] { document.lighting(invalid); });
        check(document.scene.lighting == changed && document.undoCount() == count, "Invalid settings mutated history");
        invalid = changed;
        invalid.shadowPoolMiB = 1;
        rejects([&] { document.lighting(invalid); });
        check(document.scene.lighting == changed && document.undoCount() == count, "Undersized pool mutated history");
    });

    test("old scenes keep canonical output and new lights save/reopen", [&] {
        const auto oldBytes = encodeScene(Scene::demo());
        check(oldBytes.find("\"lighting\"") == std::string::npos, "Default lighting changed M1 output");
        check(encodeScene(decodeScene(oldBytes)) == oldBytes, "Old scene canonical roundtrip changed bytes");

        SceneDocument document;
        auto sun = object("Sun");
        sun.directionalLight = DirectionalLight{};
        sun.directionalLight->color = {0.8f, 0.9f, 1.0f};
        sun.directionalLight->intensity = 4.5f;
        document.create(sun);
        auto point = object("Point");
        point.pointLight = PointLight{};
        point.pointLight->radius = 12.5f;
        document.create(point);
        auto settings = document.scene.lighting;
        settings.shadowDistance = 80.0f;
        settings.ambient = 0.15f;
        document.lighting(settings);
        const auto path = root / L"lights.scene.json";
        document.save(path);
        const auto bytes = readDocument(path);
        check(bytes.find("\"lighting\"") != std::string::npos &&
                  bytes.find("\"directionalLight\"") != std::string::npos &&
                  bytes.find("\"pointLight\"") != std::string::npos,
              "New light scene omitted JSON components");
        SceneDocument reopened;
        reopened.load(path);
        check(reopened.scene.directionalLights().size() == 1 && reopened.scene.pointLights().size() == 1,
              "Reopened light pools missing items");
        check(reopened.scene.lighting == settings, "Reopened lighting settings changed");
        check(encodeScene(reopened.scene) == bytes, "New light scene was not canonical after reopen");
        const auto copy = root / L"lights-copy.scene.json";
        document.save(copy);
        check(readDocument(path) == bytes, "Save As changed the original light scene");
        SceneDocument copied;
        copied.load(copy);
        check(copied.scene.directionalLights().size() == 1 && copied.scene.pointLights().size() == 1 &&
                  copied.scene.lighting == settings,
              "Save As lost light data");
    });

    test("invalid light JSON is rejected without loading mutation", [&] {
        SceneDocument document;
        const auto path = root / L"invalid-base.scene.json";
        document.save(path);
        const auto before = encodeScene(document.scene);
        const auto history = document.undoCount();
        const auto oldPath = document.path();

        Scene lightScene;
        auto point = object("Point");
        point.pointLight = PointLight{};
        point.pointLight->intensity = 31.25f;
        lightScene.create(point);
        auto settings = lightScene.lighting;
        settings.ambient = 0.1f;
        lightScene.lighting = settings;
        auto bytes = encodeScene(lightScene);
        auto badValue = replaceOnce(bytes, "\"intensity\": 31.25", "\"intensity\": 100001.0");
        rejects([&] { decodeScene(badValue); });
        auto badType = replaceOnce(bytes, "\"shadowResolution\": 512", "\"shadowResolution\": 512.0");
        rejects([&] { decodeScene(badType); });
        auto badKey = replaceOnce(bytes, "\"lighting\": {", "\"lighting\": {\"unknown\": 1,");
        rejects([&] { decodeScene(badKey); });
        auto duplicate = replaceOnce(bytes, "\"pointLight\": {", "\"pointLight\": {\"radius\": 8.0,");
        rejects([&] { decodeScene(duplicate); });

        put(path, badKey);
        rejects([&] { document.load(path); });
        check(encodeScene(document.scene) == before && document.undoCount() == history && document.path() == oldPath,
              "Failed light load published partial state");
    });

    std::ofstream report(root.parent_path() / "m3-scene-contract.json");
    report << "{\"passed\":" << passed << ",\"failed\":" << failed << "}\n";
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
