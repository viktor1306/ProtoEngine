#include "editor/SceneDocument.hpp"
#include <windows.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
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
void closeMatrix(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            check(std::abs(a[c][r] - b[c][r]) < 0.0003f, "Matrix mismatch");
}
EntityRecord object(std::string name = "Куб") {
    EntityRecord r;
    r.id = EntityId::create();
    r.name = std::move(name);
    r.mesh = MeshRenderer{};
    return r;
}
void put(const std::filesystem::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
    check(bool(out), "Fixture write failed");
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "Fixture token missing");
    text.replace(at, from.size(), to);
    return text;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    const auto root = (argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("test-results")) /
                      L"Сцени з пробілами" / Uuid::create().string();
    std::filesystem::create_directories(root);
    unsigned passed{}, failed{};
    auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("canonical UUID and distinct ID types", [&] {
        const auto id = EntityId::create();
        check(EntityId::parse(id.string()) == id, "UUID roundtrip");
        rejects([] { EntityId::parse("00000000-0000-0000-0000-000000000000"); });
        rejects([] { EntityId::parse("AAAAAAAA-1111-4111-8111-111111111111"); });
        static_assert(!std::is_convertible_v<EntityId, AssetId>);
    });
    test("dense removal and stale handles including load replacement", [&] {
        Scene s;
        const auto a = s.create(object()), b = s.create(object());
        const auto id = s.entity(b).id;
        s.eraseSubtree(a);
        check(s.entity(b).id == id, "Sparse mapping after swap erase");
        const auto c = s.create(object());
        check(c.slot == a.slot && c.generation != a.generation, "Generation not advanced");
        rejects([&] { s.entity(a); });
        const auto encoded = encodeScene(s);
        s = decodeScene(encoded);
        rejects([&] { s.entity(b); });
    });
    test("parent rotation, non-uniform and negative scale with exact bounds", [&] {
        Scene s;
        auto p = object("Батько");
        p.transform.position = {2, 1, -3};
        p.transform.scale = {-2, 3, 0.5f};
        p.transform.rotation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0));
        const auto parent = s.create(p);
        auto c = object("Дитина");
        c.parent = p.id;
        c.transform.position = {1, 0.5f, -2};
        const auto child = s.create(c);
        s.update();
        closeMatrix(s.transform(child).world, matrix(p.transform) * matrix(c.transform));
        for (int x : {-1, 1})
            for (int y : {-1, 1})
                for (int z : {-1, 1}) {
                    const glm::vec3 point(s.transform(child).world *
                                          glm::vec4(float(x) * .5f, float(y) * .5f, float(z) * .5f, 1));
                    check(glm::all(glm::greaterThanEqual(point, s.transform(child).bounds.min - glm::vec3(.0001f))) &&
                              glm::all(glm::lessThanEqual(point, s.transform(child).bounds.max + glm::vec3(.0001f))),
                          "Bounds miss a transformed corner");
                }
        auto other = s.create(object());
        s.update();
        const auto evaluations = s.transformEvaluations();
        s.setTransform(parent, Transform{{3, 1, -3}, p.transform.rotation, p.transform.scale});
        check(s.transformEvaluations() - evaluations == 2, "Unrelated transform recomputed");
        const auto unchanged = s.transformEvaluations();
        s.update();
        check(s.transformEvaluations() == unchanged, "Idle transforms recomputed");
        (void)other;
    });
    test("cycles, unknown IDs and duplicate IDs rejected", [&] {
        Scene s;
        auto p = object();
        const auto parent = s.create(p);
        auto c = object();
        c.parent = p.id;
        const auto child = s.create(c);
        rejects([&] { s.reparent(parent, child, false); });
        check(!s.entity(parent).parent, "Rejected cycle mutated scene");
        rejects([&] { s.create(p); });
        auto missing = object();
        missing.parent = EntityId::create();
        rejects([&] { s.create(missing); });
        auto snapshot = s.snapshot();
        snapshot.entities[0].parent = snapshot.entities[1].id;
        snapshot.entities[1].parent = snapshot.entities[0].id;
        rejects([&] { Scene::fromSnapshot(snapshot); });
    });
    test("reparent keeps world transform or rejects shear/singular parents", [&] {
        Scene s;
        auto p = object();
        p.transform.position = {4, 2, -1};
        p.transform.rotation = glm::angleAxis(.3f, glm::vec3(0, 1, 0));
        p.transform.scale = glm::vec3(2);
        const auto parent = s.create(p);
        const auto child = s.create(object());
        s.update();
        const auto original = s.transform(child).world;
        s.reparent(child, parent, true);
        closeMatrix(s.transform(child).world, original);
        s.reparent(child, {}, true);
        closeMatrix(s.transform(child).world, original);
        p.transform.scale = {2, 1, 1};
        s.setTransform(parent, p.transform);
        rejects([&] { s.reparent(child, parent, true); });
        check(!s.entity(child).parent, "Shear reparent mutated graph");
        p.transform.scale = {0, 1, 1};
        s.setTransform(parent, p.transform);
        rejects([&] { s.reparent(child, parent, true); });
        s.reparent(child, parent, false);
        check(s.transform(child).degenerate, "Zero scale not treated as degenerate");
    });
    test("strict finite transforms and camera ranges", [&] {
        Scene s;
        auto r = object();
        r.transform.rotation = {0, 0, 0, 0};
        rejects([&] { s.create(r); });
        r = object();
        r.transform.position.x = std::numeric_limits<float>::infinity();
        rejects([&] { s.create(r); });
        r = object();
        r.camera = Camera{};
        r.camera->nearPlane = r.camera->farPlane;
        rejects([&] { s.create(r); });
        r = object();
        r.transform.rotation = {2, 0, 0, 0};
        const auto h = s.create(r);
        check(std::abs(glm::length(s.transform(h).local.rotation) - 1) < 1e-6f, "Quaternion not normalized");
    });
    test("save/load hierarchy and active camera retain UUIDs and properties", [&] {
        auto s = Scene::demo();
        const auto a = s.meshes()[0], b = s.meshes()[1];
        s.reparent(a, b, false);
        auto r = s.record(a);
        r.name = "Дитина Ґґ Єє Іі Її";
        r.mesh->color = {.2f, .3f, .9f, 1};
        sdk::BehaviorBinding behavior;
        behavior.id = BehaviorBindingId::create();
        behavior.type = BehaviorTypeId::create();
        behavior.properties.emplace("speed", 2.5);
        r.behaviors.push_back(behavior);
        s.edit(r);
        const auto bytes = encodeScene(s);
        auto loaded = decodeScene(bytes);
        check(encodeScene(loaded) == bytes && loaded.activeCamera == s.activeCamera &&
                  loaded.record(loaded.find(r.id)).behaviors == r.behaviors,
              "Scene roundtrip changed data");
        closeMatrix(loaded.transform(loaded.find(r.id)).world, s.transform(a).world);
    });
    test("malformed, duplicate fields, future version and unknown resource rejection", [&] {
        const auto bytes = encodeScene(Scene::demo());
        rejects([&] { decodeScene("{oops"); });
        rejects([&] { decodeScene(replace(bytes, "\"formatVersion\": 1", "\"formatVersion\": 2")); });
        rejects(
            [&] { decodeScene(replace(bytes, "\"formatVersion\": 1", "\"formatVersion\": 1, \"formatVersion\": 1")); });
        rejects([&] { decodeScene(replace(bytes, builtin::cube.string(), AssetId::create().string())); });
        rejects([&] { decodeScene(replace(bytes, "\"behaviors\": []", "\"behaviors\": [{}]")); });
        auto s = Scene::demo().snapshot();
        s.entities.push_back(s.entities.front());
        rejects([&] { Scene::fromSnapshot(s); });
        s = Scene::demo().snapshot();
        s.activeCamera = EntityId::create();
        rejects([&] { Scene::fromSnapshot(s); });
    });
    test("delete subtree, Undo/Redo, stable IDs and fresh handles", [&] {
        SceneDocument d;
        auto p = object("Parent");
        const auto pid = d.create(p);
        auto c = object("Child");
        c.parent = pid;
        const auto cid = d.create(c);
        const auto stale = d.scene.find(cid);
        d.erase(pid);
        check(!d.scene.find(cid), "Child survived deletion");
        d.undo();
        check(d.scene.find(cid) && d.scene.record(d.scene.find(cid)).parent == pid, "Undo lost hierarchy");
        rejects([&] { d.scene.entity(stale); });
        d.redo();
        check(!d.scene.find(pid), "Redo failed");
        d.undo();
        check(bool(d.scene.find(cid)), "Second Undo changed IDs");
    });
    test("one drag is one Undo and saved-state markers survive branching", [&] {
        SceneDocument d;
        const auto id = d.create(object());
        const auto file = root / L"жест.scene.json";
        d.save(file);
        check(!d.dirty(), "Save did not clear dirty state");
        const auto steps = d.undoCount();
        auto r = d.scene.record(d.scene.find(id));
        d.beginGesture(id);
        for (int i = 1; i <= 25; ++i) {
            r.transform.position.x = float(i);
            d.preview(r);
        }
        check(d.dirty(), "Live gesture not dirty");
        d.endGesture();
        check(d.undoCount() == steps + 1, "Gesture produced multiple commands");
        d.undo();
        check(!d.dirty() && d.scene.transform(d.scene.find(id)).local.position.x == 0,
              "Undo did not return to saved state");
        d.redo();
        d.save(file);
        d.undo();
        check(d.dirty(), "Undo after save must be dirty");
        r = d.scene.record(d.scene.find(id));
        r.transform.position.z = 3;
        d.edit(r);
        check(!d.canRedo(), "New branch kept old redo");
    });
    test("atomic replacement preserves exact previous bytes", [&] {
        SceneDocument d;
        const auto path = root / L"резервна копія.scene.json";
        d.save(path);
        const auto before = readDocument(path);
        const auto id = d.create(object());
        d.save(path);
        auto backup = path;
        backup += L".bak";
        check(readDocument(backup) == before && !d.dirty(), "Backup mismatch");
        d.load(path);
        check(bool(d.scene.find(id)), "Saved entity missing");
    });
    test("failed write preserves original bytes, path and dirty state", [&] {
        SceneDocument d;
        const auto path = root / L"заблокована.scene.json";
        d.save(path);
        const auto before = readDocument(path);
        d.create(object());
        const HANDLE lock =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "Could not lock fixture");
        rejects([&] { d.save(path); });
        CloseHandle(lock);
        check(d.dirty() && readDocument(path) == before && d.path() == std::filesystem::absolute(path),
              "Failed save lost state");
        d.save(path);
        check(!d.dirty(), "Save could not recover after unlocking");
    });
    test("failed load preserves live scene, path and Undo history", [&] {
        SceneDocument d;
        d.save(root / L"чинна.scene.json");
        d.create(object());
        const auto before = encodeScene(d.scene);
        const auto history = d.undoCount();
        const auto path = d.path();
        const auto broken = root / L"пошкоджена.scene.json";
        put(broken, "{broken");
        rejects([&] { d.load(broken); });
        check(encodeScene(d.scene) == before && d.path() == path && d.undoCount() == history && d.dirty(),
              "Failed load published partial state");
    });
    test("external document change is detected before overwrite", [&] {
        SceneDocument d;
        const auto file = root / L"зовнішня зміна.scene.json";
        d.save(file);
        const auto external = encodeScene(Scene::demo());
        put(file, external);
        d.create(object());
        rejects([&] { d.save(file); });
        check(readDocument(file) == external && d.dirty(), "External contents overwritten");
    });
    test("picking negative scale and disabled hierarchy", [&] {
        Scene s;
        auto r = object();
        r.transform.scale = {-2, 1, .5f};
        const auto h = s.create(r);
        s.update();
        const auto hit = s.pick({{0, 0, 4}, {0, 0, -1}});
        check(hit && hit->id == r.id && std::abs(hit->distance - 3.75f) < 1e-5f, "Negative scale pick failed");
        r.enabled = false;
        s.edit(r);
        check(!s.pick({{0, 0, 4}, {0, 0, -1}}), "Disabled mesh is selectable");
        (void)h;
    });
    test("no-op gesture preserves history and ordinary edits preserve sibling order", [&] {
        SceneDocument d;
        EntityRecord p = object("Parent");
        p.mesh.reset();
        const auto parent = d.create(p);
        auto a = object("A"), b = object("B");
        a.parent = b.parent = parent;
        const auto aid = d.create(a);
        d.create(b);
        const auto before = d.scene.entity(d.scene.find(parent)).children;
        auto r = d.scene.record(d.scene.find(aid));
        r.name = "A edited";
        d.edit(r);
        check(d.scene.entity(d.scene.find(parent)).children == before, "Property edit reordered siblings");
        d.save(root / L"без змін.scene.json");
        const auto count = d.undoCount();
        d.beginGesture(aid);
        d.preview(r);
        d.endGesture();
        check(d.undoCount() == count && !d.dirty(), "No-op drag polluted history");
        p.enabled = false;
        d.scene.edit(p);
        check(!d.scene.transform(d.scene.find(aid)).visible, "Parent visibility not propagated");
    });
    test("empty existing save target and foreign lock file are preserved correctly", [&] {
        SceneDocument d;
        const auto path = root / L"початково порожня.scene.json";
        put(path, "");
        d.save(path);
        auto backup = path;
        backup += L".bak";
        check(readDocument(backup).empty(), "Empty previous document not backed up");
        const auto before = readDocument(path);
        auto lock = path;
        lock += L".lock";
        put(lock, "foreign lock");
        d.create(object());
        rejects([&] { d.save(path); });
        check(readDocument(lock) == "foreign lock" && readDocument(path) == before && d.dirty(),
              "Save changed foreign lock or original document");
    });
    test("invalid UTF-8 rejected and small nonzero objects remain renderable", [&] {
        Scene s;
        auto r = object();
        r.name = std::string(1, char(0xff));
        rejects([&] { s.create(r); });
        r = object();
        r.transform.scale = glm::vec3(.001f);
        const auto h = s.create(r);
        s.update();
        check(!s.transform(h).degenerate, "Millimeter object incorrectly treated as zero scale");
    });
    test("picking agrees with one-sided geometry and near-plane clipping", [&] {
        Scene s;
        auto r = object();
        s.create(r);
        s.update();
        check(!s.pick({{0, 0, 0}, {0, 0, 1}}), "Invisible backfaces selected from inside cube");
        s.eraseSubtree(s.find(r.id));
        r = object();
        r.mesh->mesh = builtin::plane;
        s.create(r);
        s.update();
        check(!s.pick({{0, -2, 0}, {0, 1, 0}}), "Invisible plane underside selected");
        check(bool(s.pick({{0, 2, 0}, {0, -1, 0}})), "Visible plane not selected");
    });
    test("Save As creates a distinct scene resource without changing entity IDs", [&] {
        SceneDocument d;
        const auto a = root / L"оригінал.scene.json", b = root / L"окрема копія.scene.json";
        d.save(a);
        const auto originalId = d.scene.id;
        const auto originalBytes = readDocument(a);
        const auto entity = d.scene.entities().front();
        const auto entityId = d.scene.entity(entity).id;
        d.save(b);
        check(d.scene.id != originalId && d.scene.find(entityId) == entity && readDocument(a) == originalBytes,
              "Save As damaged identity or original file");
        const auto copiedId = d.scene.id;
        d.save(b);
        check(d.scene.id == copiedId && !d.dirty(), "Ordinary save changed scene identity");
        const auto locked = root / L"невдала копія.scene.json";
        put(locked, originalBytes);
        const auto handle =
            CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Fixture lock failed");
        rejects([&] { d.save(locked); });
        CloseHandle(handle);
        check(d.scene.id == copiedId && d.path() == std::filesystem::absolute(b),
              "Failed Save As published a new identity/path");
    });
    std::ofstream report(root.parent_path().parent_path() / "m1-scene-contract.json");
    report << "{\"passed\":" << passed << ",\"failed\":" << failed << "}\n";
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
