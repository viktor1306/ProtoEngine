#include "editor/Editor.hpp"
#include "editor/FileDialog.hpp"
#include "renderer/AssetRenderer.hpp"
#include "project/ProjectSession.hpp"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace proto {
namespace {
constexpr ImVec4 accent{.33f, .80f, .70f, 1};
constexpr auto hierarchyTitle = "Ієрархія###Hierarchy";
constexpr auto inspectorTitle = "Властивості###Inspector";
constexpr auto filesTitle = "Файли проєкту###Files";
bool anyPopup() {
    return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}
} // namespace
void Editor::showError(const std::exception& e) {
    error_ = e.what();
    showError_ = true;
}
void Editor::requestClose() {
    if (playBusy())
        stopPlay();
    if (codeDirty()) {
        showError(std::runtime_error("Збережіть зміни у панелі C++ перед закриттям редактора"));
        return;
    }
    request(Action::Close);
}
void Editor::request(Action action) {
    try {
        document_.endGesture();
    } catch (const std::exception& e) {
        showError(e);
        return;
    }
    const bool untouchedStarter = !project_ && document_.path().empty() && document_.undoCount() == 0 &&
                                  (action == Action::NewProject || action == Action::OpenProject);
    if (document_.dirty() && !untouchedStarter) {
        pending_ = action;
        showConfirm_ = true;
    } else
        perform(action);
}
void Editor::perform(Action action) {
    try {
        if (action == Action::New) {
            document_.newScene(bool(project_));
            if (project_)
                document_.bindProjectWorkspace(project_->assetWorkspace());
            camera_ = {};
            useSceneCamera_ = false;
        }
        if (action == Action::Open)
            if (const auto path =
                    sceneFileDialog(window_, false, project_ ? project_->root() / "Scenes" : std::filesystem::path{})) {
                if (project_)
                    openProjectScene(*path);
                else
                    document_.load(*path);
                camera_ = {};
                useSceneCamera_ = false;
                status_ = "Сцену відкрито";
            }
        if (action == Action::Close)
            closeApproved_ = true;
        if (action == Action::NewProject || action == Action::OpenProject || action == Action::OpenFileScene)
            performProjectAction(action);
    } catch (const std::exception& e) {
        showError(e);
    }
}
bool Editor::saveScene(bool saveAs) {
    try {
        auto path = document_.path();
        if (saveAs || path.empty()) {
            const auto selected =
                sceneFileDialog(window_, true, project_ ? project_->root() / "Scenes" : std::filesystem::path{});
            if (!selected)
                return false;
            path = *selected;
        }
        if (project_)
            document_.saveProject(path, project_->root());
        else
            document_.save(path);
        status_ = "Сцену збережено";
        return true;
    } catch (const std::exception& e) {
        showError(e);
        return false;
    }
}
void Editor::addObject(int kind) {
    try {
        EntityRecord record;
        if (kind == 0) {
            record.name = "Куб";
            record.mesh = MeshRenderer{};
            record.transform.position = {1.5f, .5f, 0};
        }
        if (kind == 1) {
            record.name = "Площина";
            record.mesh = MeshRenderer{builtin::plane, {.16f, .20f, .25f, 1}};
            record.transform.scale = {5, 1, 5};
        }
        if (kind == 2)
            record.name = "Порожній об’єкт";
        if (kind == 3) {
            record.name = "Камера";
            record.camera = Camera{};
            record.transform.position = camera_.eye();
            record.transform.rotation =
                glm::quatLookAt(glm::normalize(camera_.pivot - camera_.eye()), glm::vec3(0, 1, 0));
        }
        document_.create(record);
    } catch (const std::exception& e) {
        showError(e);
    }
}
void Editor::focusSelection() {
    const auto h = document_.scene.find(document_.selection);
    if (!h)
        return;
    document_.scene.update();
    auto bounds = document_.scene.transform(h).bounds;
    for (const auto& child : document_.scene.subtree(h)) {
        const auto ch = document_.scene.find(child.id);
        if (!document_.scene.mesh(ch))
            continue;
        const auto& b = document_.scene.transform(ch).bounds;
        bounds.min = glm::min(bounds.min, b.min);
        bounds.max = glm::max(bounds.max, b.max);
    }
    camera_.focus(bounds);
    useSceneCamera_ = false;
}
void Editor::sceneMenu() {
    if (playBusy()) {
        ImGui::TextDisabled("%s%s", document_.scene.name.c_str(), document_.dirty() ? " *" : "");
        return;
    }
    if (ImGui::BeginMenu("Редагування")) {
        if (ImGui::MenuItem("Скасувати", "Ctrl+Z", false, document_.canUndo() && !importJob_.busy() && !gizmoActive()))
            try {
                document_.undo();
            } catch (const std::exception& e) {
                showError(e);
            }
        if (ImGui::MenuItem("Повторити", "Ctrl+Y", false, document_.canRedo() && !importJob_.busy() && !gizmoActive()))
            try {
                document_.redo();
            } catch (const std::exception& e) {
                showError(e);
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Видалити об’єкт і нащадків", "Delete", false, bool(document_.selection)))
            document_.erase(document_.selection);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Додати")) {
        if (ImGui::MenuItem("Куб"))
            addObject(0);
        if (ImGui::MenuItem("Площину"))
            addObject(1);
        if (ImGui::MenuItem("Порожній об’єкт"))
            addObject(2);
        if (ImGui::MenuItem("Камеру"))
            addObject(3);
        if (ImGui::MenuItem("Сонце"))
            addLight(false);
        if (ImGui::MenuItem("Точкове світло"))
            addLight(true);
        ImGui::EndMenu();
    }
    auto& io = ImGui::GetIO();
    if (!anyPopup() && !gizmoActive()) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (codeInputFocused())
                saveCode();
            else
                saveScene(io.KeyShift);
        }
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
                request(Action::New);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
                request(Action::Open);
            try {
                if (!importJob_.busy() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                    if (io.KeyShift)
                        document_.redo();
                    else
                        document_.undo();
                }
                if (!importJob_.busy() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                    document_.redo();
                if (!fileInputFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                    document_.erase(document_.selection);
                if (!fileInputFocused() && ImGui::IsKeyPressed(ImGuiKey_F, false))
                    focusSelection();
            } catch (const std::exception& e) {
                showError(e);
            }
        }
    }
    ImGui::Separator();
    const auto name = document_.path().empty() ? document_.scene.name : utf8(document_.path().filename().wstring());
    ImGui::TextDisabled("%s%s", name.c_str(), document_.dirty() ? " *" : "");
    const std::string title = (project_ ? project_->info().name + " / " : "") + name + (document_.dirty() ? " *" : "") +
                              " — Proto Engine · M6";
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }
    ImGui::Separator();
}
void Editor::reparentObject(EntityId child, EntityId parent) {
    try {
        document_.reparent(child, parent, true);
    } catch (const std::exception& e) {
        error_ = e.what();
        if (error_.find("shear") != std::string::npos || error_.find("масштаб") != std::string::npos ||
            error_.find("матриця") != std::string::npos)
            reparentFallback_ = {{child, parent}};
        else
            showError(e);
    }
}
void Editor::sceneNode(EntityHandle handle) {
    const auto& entity = document_.scene.entity(handle);
    const auto id = entity.id;
    const auto key = id.string();
    ImGui::PushID(key.c_str());
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (entity.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (document_.selection == id)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (!entity.enabled)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool opened = ImGui::TreeNodeEx("node", flags, "%s", entity.name.c_str());
    if (!entity.enabled)
        ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        document_.endGesture();
        document_.selection = id;
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("PROTO_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(entity.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("PROTO_ENTITY"))
            if (payload->DataSize == sizeof(EntityId)) {
                const auto child = *static_cast<const EntityId*>(payload->Data);
                queuedEdit_ = [this, child, id] { reparentObject(child, id); };
            }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Фокус", "F")) {
            document_.selection = id;
            focusSelection();
        }
        if (ImGui::MenuItem("Додати дочірній об’єкт"))
            queuedEdit_ = [this, id] {
                EntityRecord child;
                child.name = "Дочірній об’єкт";
                child.parent = id;
                document_.create(child);
            };
        if (ImGui::MenuItem("Перемістити в корінь", nullptr, false, bool(entity.parent)))
            queuedEdit_ = [this, id] { reparentObject(id, {}); };
        if (ImGui::MenuItem("Видалити піддерево", "Delete"))
            queuedEdit_ = [this, id] { document_.erase(id); };
        ImGui::EndPopup();
    }
    if (opened) {
        for (const auto child : entity.children)
            sceneNode(child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}
void Editor::sceneHierarchy() {
    ImGui::Begin(hierarchyTitle);
    ImGui::TextDisabled("Об’єктів: %zu", document_.scene.entities().size());
    if (ImGui::SmallButton("+ Куб"))
        addObject(0);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Порожній"))
        addObject(2);
    ImGui::Separator();
    ImGui::Selectable("Корінь сцени", false);
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("PROTO_ENTITY"))
            if (payload->DataSize == sizeof(EntityId)) {
                const auto child = *static_cast<const EntityId*>(payload->Data);
                queuedEdit_ = [this, child] { reparentObject(child, {}); };
            }
        ImGui::EndDragDropTarget();
    }
    for (const auto handle : document_.scene.entities())
        if (!document_.scene.entity(handle).parent)
            sceneNode(handle);
    ImGui::End();
    if (queuedEdit_) {
        auto edit = std::move(queuedEdit_);
        queuedEdit_ = {};
        try {
            edit();
        } catch (const std::exception& e) {
            showError(e);
        }
    }
}
void Editor::propertyGesture(bool changed, const EntityRecord& record) {
    try {
        if (ImGui::IsItemActivated())
            document_.beginGesture(record.id);
        if (changed)
            document_.preview(record);
        if (ImGui::IsItemDeactivated())
            document_.endGesture();
    } catch (const std::exception& e) {
        document_.endGesture();
        showError(e);
    }
}
void Editor::sceneInspector() {
    ImGui::Begin(inspectorTitle);
    const auto handle = document_.scene.find(document_.selection);
    if (!handle) {
        ImGui::TextWrapped("Оберіть об’єкт у сцені або ієрархії.");
        ImGui::End();
        return;
    }
    auto r = document_.scene.record(handle);
    ImGui::PushID(r.id.string().c_str());
    ImGui::PushItemWidth(-1);
    char name[1025]{};
    std::memcpy(name, r.name.data(), std::min(r.name.size(), sizeof(name) - 1));
    const bool renamed = ImGui::InputText("##Name", name, sizeof(name));
    // Allow clearing the text while typing a replacement; only publish valid names.
    const bool validRename = renamed && name[0] != '\0';
    if (validRename)
        r.name = name;
    propertyGesture(validRename, r);
    if (ImGui::Checkbox("Увімкнено", &r.enabled)) {
        try {
            document_.edit(r);
        } catch (const std::exception& e) {
            showError(e);
        }
    }
    ImGui::SeparatorText("Локальна трансформація");
    ImGui::TextDisabled("Позиція XYZ, м");
    const bool moved = ImGui::DragFloat3("##Position", &r.transform.position.x, .02f, 0, 0, "%.3f");
    positionInputMin_ = ImGui::GetItemRectMin();
    positionInputMax_ = ImGui::GetItemRectMax();
    propertyGesture(moved, r);
    glm::vec3 angles = glm::degrees(glm::eulerAngles(r.transform.rotation));
    ImGui::TextDisabled("Обертання, градуси");
    const bool rotated = ImGui::DragFloat3("##Rotation", &angles.x, .4f, 0, 0, "%.2f");
    if (rotated)
        r.transform.rotation = glm::quat(glm::radians(angles));
    propertyGesture(rotated, r);
    ImGui::TextDisabled("Масштаб");
    propertyGesture(ImGui::DragFloat3("##Scale", &r.transform.scale.x, .01f, 0, 0, "%.3f"), r);
    ImGui::TextDisabled("Батько");
    const auto parent = document_.scene.find(r.parent);
    const auto parentName = parent ? document_.scene.entity(parent).name : "Корінь сцени";
    if (ImGui::BeginCombo("##Parent", parentName.c_str())) {
        if (ImGui::Selectable("Корінь сцени", !r.parent))
            reparentObject(r.id, {});
        for (const auto h : document_.scene.entities()) {
            const auto& e = document_.scene.entity(h);
            if (e.id == r.id)
                continue;
            ImGui::PushID(e.id.string().c_str());
            if (ImGui::Selectable(e.name.c_str(), e.id == r.parent))
                reparentObject(r.id, e.id);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (r.mesh) {
        ImGui::SeparatorText("Геометрія");
        const bool imported = document_.scene.assets->meshes.contains(r.mesh->mesh);
        ImGui::TextUnformatted(imported ? document_.scene.assets->meshes.at(r.mesh->mesh)->name.c_str()
                               : r.mesh->mesh == builtin::cube ? "Вбудований куб"
                                                               : "Вбудована площина");
        ImGui::TextDisabled("Колір");
        propertyGesture(ImGui::ColorEdit3("##Color", &r.mesh->color.r), r);
        if (imported)
            materialInspector(*r.mesh);
        else
            ImGui::TextDisabled(useStudioLighting_ ? "Режим: Unlit" : "Матеріал: PBR · шорсткість 0.65");
        propertyGesture(ImGui::Checkbox("Відкидати тіні", &r.mesh->castShadows), r);
        propertyGesture(ImGui::Checkbox("Приймати тіні", &r.mesh->receiveShadows), r);
    }
    if (r.camera) {
        ImGui::SeparatorText("Камера сцени");
        ImGui::TextDisabled("Вертикальний FOV");
        propertyGesture(ImGui::DragFloat("##Fov", &r.camera->verticalFovDegrees, .2f, 5, 150, "%.1f°"), r);
        ImGui::TextDisabled("Ближня площина");
        propertyGesture(ImGui::DragFloat("##Near", &r.camera->nearPlane, .001f, .001f, 10, "%.3f"), r);
        ImGui::TextDisabled("Дальня площина");
        propertyGesture(ImGui::DragFloat("##Far", &r.camera->farPlane, 1, 1, 1000000, "%.1f"), r);
        if (document_.scene.activeCamera == r.id)
            ImGui::TextColored(accent, "Активна камера сцени");
        else if (ImGui::Button("Зробити активною"))
            document_.setActiveCamera(r.id);
    }
    lightInspector(r);
    behaviorInspector();
    ImGui::SeparatorText("Об’єкт");
    if (ImGui::Button("Фокус", ImVec2(-1, 0)))
        focusSelection();
    ImGui::TextDisabled("UUID");
    ImGui::TextWrapped("%s", r.id.string().c_str());
    if (document_.scene.transform(handle).degenerate)
        ImGui::TextWrapped("Нульовий масштаб: геометрія пропускається.");
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::End();
}
void Editor::sceneFiles() {
    ImGui::Begin(filesTitle);
    if (ImGui::Button("Відкрити сцену…"))
        request(Action::Open);
    ImGui::SameLine();
    if (ImGui::Button("Зберегти"))
        saveScene();
    ImGui::SameLine();
    if (ImGui::Button("Зберегти як…"))
        saveScene(true);
    ImGui::SameLine();
    ImGui::BeginDisabled(importJob_.busy());
    if (ImGui::Button("Імпорт glTF / GLB…"))
        importModel();
    ImGui::EndDisabled();
    ImGui::Separator();
    if (document_.path().empty())
        ImGui::TextUnformatted("Нова сцена — оберіть файл для збереження.");
    else {
        ImGui::TextDisabled("ВІДКРИТИЙ ФАЙЛ");
        ImGui::TextWrapped("%s", utf8(document_.path().wstring()).c_str());
    }
    ImGui::TextColored(document_.dirty() ? ImVec4(1, .74f, .3f, 1) : accent, "%s",
                       document_.dirty() ? "Є незбережені зміни" : "Усі зміни збережено");
    if (!status_.empty())
        ImGui::TextDisabled("%s", status_.c_str());
    if (importJob_.busy())
        ImGui::TextColored(accent, "Підготовка моделі… Можна продовжувати роботу зі сценою.");
    const auto& gpu = *renderer_.assetRenderer();
    const auto& lighting = gpu.lightingStats();
    ImGui::TextDisabled(
        "Світло: %u  |  Тіньові проходи: %u  |  Кеш: %u  |  Пакети: %u  |  Тіні: %.1f MiB  |  Tile overflow: %u",
        lighting.lights, lighting.shadowFaces, lighting.shadowCacheHits, lighting.batches,
        double(lighting.shadowBytes) / (1024 * 1024), lighting.tileOverflows);
    if (gpu.pending())
        ImGui::Text("Завантаження на GPU: %zu ресурсів · %.2f MiB / кадр", gpu.pending(),
                    double(gpu.uploadedBytes()) / (1024 * 1024));
    if (!gpu.error().empty())
        ImGui::TextWrapped("Помилка GPU: %s", gpu.error().c_str());
    ImGui::BeginDisabled(importJob_.busy());
    for (const auto& [id, model] : document_.scene.assets->models) {
        ImGui::PushID(id.string().c_str());
        ImGui::TextUnformatted(model->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("+ На сцену"))
            try {
                document_.instantiate(model);
                focusSelection();
            } catch (const std::exception& e) {
                showError(e);
            }
        ImGui::SameLine();
        if (ImGui::SmallButton("Оновити з файлу"))
            try {
                document_.endGesture();
                importScene_ = document_.scene.id;
                importInstance_ = false;
                importJob_.reload(document_.workspace(), id);
            } catch (const std::exception& e) {
                showError(e);
            }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", model->source.c_str());
        for (const auto& warning : model->warnings)
            ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Undo: %zu  |  Історія: %.1f KiB  |  GPU: %.2f ms", document_.undoCount(),
                        static_cast<double>(document_.historyBytes()) / 1024, renderer_.gpuMilliseconds());
    ImGui::End();
}
void Editor::scenePopups() {
    if (showConfirm_) {
        ImGui::OpenPopup("Незбережені зміни");
        showConfirm_ = false;
    }
    if (ImGui::BeginPopupModal("Незбережені зміни", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Зберегти зміни поточної сцени?");
        if (ImGui::Button("Зберегти")) {
            if (saveScene()) {
                const auto action = pending_;
                pending_ = Action::None;
                ImGui::CloseCurrentPopup();
                perform(action);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Не зберігати")) {
            const auto action = pending_;
            pending_ = Action::None;
            ImGui::CloseCurrentPopup();
            perform(action);
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати")) {
            pending_ = Action::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (showError_) {
        ImGui::OpenPopup("Помилка сцени");
        showError_ = false;
    }
    if (ImGui::BeginPopupModal("Помилка сцени", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480 * appliedScale_);
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Гаразд"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (reparentFallback_ && !ImGui::IsPopupOpen("Зміна батька"))
        ImGui::OpenPopup("Зміна батька");
    if (ImGui::BeginPopupModal("Зміна батька", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480 * appliedScale_);
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Зберегти локальні параметри")) {
            const auto [child, parent] = *reparentFallback_;
            reparentFallback_.reset();
            ImGui::CloseCurrentPopup();
            try {
                document_.reparent(child, parent, false);
            } catch (const std::exception& e) {
                showError(e);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати")) {
            reparentFallback_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
void Editor::sceneViewport(ImVec2 position, ImVec2 size) {
    viewportPosition_ = position;
    viewportSize_ = size;
    auto& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered() && !gizmoActive() && !gizmoInputConsumed_;
    if (hovered && !anyPopup()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            useSceneCamera_ = false;
            camera_.yaw -= io.MouseDelta.x * .006f;
            camera_.pitch = std::clamp(camera_.pitch + io.MouseDelta.y * .006f, -1.5f, 1.5f);
        }
        const auto inverseView = glm::inverse(camera_.view());
        const glm::vec3 right(inverseView[0]), up(inverseView[1]), forward(-inverseView[2]);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            useSceneCamera_ = false;
            camera_.pivot += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * camera_.distance * .0018f;
        }
        if (io.MouseWheel != 0) {
            useSceneCamera_ = false;
            camera_.distance = std::clamp(camera_.distance * std::pow(.85f, io.MouseWheel), .3f, 10000.0f);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            const float speed = std::min(io.DeltaTime, .05f) * (io.KeyShift ? 12 : 4);
            if (ImGui::IsKeyDown(ImGuiKey_W))
                camera_.pivot += forward * speed;
            if (ImGui::IsKeyDown(ImGuiKey_S))
                camera_.pivot -= forward * speed;
            if (ImGui::IsKeyDown(ImGuiKey_D))
                camera_.pivot += right * speed;
            if (ImGui::IsKeyDown(ImGuiKey_A))
                camera_.pivot -= right * speed;
            if (ImGui::IsKeyDown(ImGuiKey_E))
                camera_.pivot.y += speed;
            if (ImGui::IsKeyDown(ImGuiKey_Q))
                camera_.pivot.y -= speed;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            prepareCameraView();
            const auto ray = viewportRay(sceneView_.viewProjection, (io.MousePos.x - position.x) / size.x,
                                         (io.MousePos.y - position.y) / size.y);
            const auto hit = document_.scene.pick(ray);
            document_.endGesture();
            document_.selection = hit ? hit->id : EntityId{};
        }
    }
}
void Editor::prepareCameraView() {
    auto& scene = document_.scene;
    scene.update();
    const auto extent = renderer_.viewportExtent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    auto view = camera_.view();
    Camera camera;
    if (useSceneCamera_) {
        const auto handle = scene.find(scene.activeCamera);
        if (handle && scene.camera(handle) && !scene.transform(handle).degenerate && scene.transform(handle).visible) {
            const auto world = scene.transform(handle).world;
            const glm::vec3 eye(world[3]);
            view = glm::lookAtRH(eye, eye - glm::normalize(glm::vec3(world[2])), glm::normalize(glm::vec3(world[1])));
            camera = *scene.camera(handle);
        } else
            useSceneCamera_ = false;
    }
    sceneView_.viewProjection =
        glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDegrees), aspect, camera.nearPlane, camera.farPlane) *
        view;
    sceneView_.eye = glm::vec3(glm::inverse(view)[3]);
    sceneView_.cameraView = view;
    sceneView_.verticalFovDegrees = camera.verticalFovDegrees;
    sceneView_.cameraNear = camera.nearPlane;
    sceneView_.cameraFar = camera.farPlane;
    sceneView_.exposure = camera.exposure;
}
void Editor::prepareSceneView(bool withProbes) {
    prepareCameraView();
    auto& scene = document_.scene;
    const auto extent = renderer_.viewportExtent();
    sceneView_.items.clear();
    sceneView_.imported.clear();
    sceneView_.depthProbes.clear();
    sceneView_.colorProbes.clear();
    sceneView_.grid = showGrid_;
    sceneView_.assets = scene.assets;
    sceneView_.exactDepth = true;
    sceneView_.lightingMode = useStudioLighting_ || assetSmoke_ ? LightingMode::Studio : LightingMode::Scene;
    sceneView_.lighting = scene.lighting;
    sceneView_.casters.clear();
    sceneView_.lights.clear();
    for (const auto handle : scene.directionalLights()) {
        const auto& t = scene.transform(handle);
        if (!t.visible || t.degenerate)
            continue;
        const auto& l = *scene.directionalLight(handle);
        sceneView_.lights.push_back({scene.entity(handle).id, glm::vec3(t.world[3]),
                                     -glm::normalize(glm::vec3(t.world[2])), l.color, l.intensity, 8, true, l.shadows});
    }
    for (const auto handle : scene.pointLights()) {
        const auto& t = scene.transform(handle);
        if (!t.visible || t.degenerate)
            continue;
        const auto& l = *scene.pointLight(handle);
        sceneView_.lights.push_back({scene.entity(handle).id, glm::vec3(t.world[3]), glm::vec3(0, -1, 0), l.color,
                                     l.intensity, l.radius, false, l.shadows});
    }
    for (const auto handle : scene.meshes()) {
        const auto& t = scene.transform(handle);
        const auto& m = *scene.mesh(handle);
        if (!t.visible || t.degenerate)
            continue;
        const bool selected = scene.entity(handle).id == document_.selection;
        ImportedItem item{m.mesh,   t.world,         t.normal, m.color, m.materials, scene.entity(handle).id,
                          t.bounds, m.receiveShadows};
        if (sceneView_.lightingMode == LightingMode::Scene && m.castShadows)
            sceneView_.casters.push_back(item);
        if (!inFrustum(t.bounds, sceneView_.viewProjection))
            continue;
        if (scene.assets->meshes.contains(m.mesh) || sceneView_.lightingMode == LightingMode::Scene) {
            sceneView_.imported.push_back(item);
            // M1's analytic box-depth oracle does not describe textured imported surfaces.
            sceneView_.exactDepth = false;
            if (selected) {
                auto wire = glm::translate(glm::mat4(1), (t.bounds.min + t.bounds.max) * .5f);
                wire = glm::scale(wire, glm::max(t.bounds.max - t.bounds.min, glm::vec3(.001f)));
                sceneView_.items.push_back({wire, glm::vec4(1), Primitive::Cube, true, true});
            }
        } else
            sceneView_.items.push_back(
                {t.world, m.color, m.mesh == builtin::cube ? Primitive::Cube : Primitive::Plane, selected});
    }
    if (withProbes && assetSmoke_)
        assetColorProbes();
    if (withProbes && lightingSmoke_)
        lightingColorProbes();
    if (!withProbes || !sceneView_.exactDepth)
        return;
    // Independent CPU ray/box oracle, evaluated only for requested captures.
    const auto probe = [&](float u, float v) {
        if (u < 0 || u >= 1 || v < 0 || v >= 1)
            return;
        u = (std::floor(u * static_cast<float>(extent.width)) + .5f) / static_cast<float>(extent.width);
        v = (std::floor(v * static_cast<float>(extent.height)) + .5f) / static_cast<float>(extent.height);
        const auto hit = scene.pick(viewportRay(sceneView_.viewProjection, u, v));
        if (!hit)
            return;
        const auto projected = sceneView_.viewProjection * glm::vec4(hit->position, 1);
        const float depth = projected.z / projected.w;
        if (depth > 0 && depth < 1)
            sceneView_.depthProbes.push_back({u, v, depth});
    };
    for (float u : {.25f, .4f, .5f, .6f, .75f})
        for (float v : {.25f, .4f, .5f, .6f, .75f})
            probe(u, v);
    if (const auto h = scene.find(document_.selection); h && scene.mesh(h)) {
        const auto& b = scene.transform(h).bounds;
        const auto p = sceneView_.viewProjection * glm::vec4((b.min + b.max) * .5f, 1);
        if (p.w > 0)
            probe(p.x / p.w * .5f + .5f, .5f - p.y / p.w * .5f);
    }
}
void Editor::sceneSmokeStep(uint64_t frame, const std::filesystem::path& path) {
    if (m0_)
        return;
    useStudioLighting_ = true;
    if (smokeStage_ == 0 && frame >= 4) {
        document_.save(path);
        auto r = document_.scene.record(document_.scene.find(document_.selection));
        r.transform.scale = {-1.4f, 1.2f, .8f};
        r.transform.rotation = glm::angleAxis(.45f, glm::vec3(0, 1, 0));
        document_.beginGesture(r.id);
        for (int i = 1; i <= 10; ++i) {
            r.transform.position.x = float(i) * .04f;
            document_.preview(r);
        }
        document_.endGesture();
        EntityRecord parent;
        parent.name = "Група Ґґ Єє Іі Її";
        parent.transform.rotation = glm::angleAxis(-.3f, glm::vec3(0, 1, 0));
        parent.transform.scale = {1.1f, 1, .9f};
        const auto group = document_.create(parent);
        document_.reparent(r.id, group, false);
        document_.erase(group);
        document_.undo();
        document_.redo();
        document_.undo();
        document_.selection = r.id;
        focusSelection();
        smokeStage_ = 1;
    }
    if (smokeStage_ == 1 && frame >= 35) {
        const auto before = encodeScene(document_.scene);
        document_.save(path);
        document_.load(path);
        if (document_.dirty() || encodeScene(document_.scene) != before)
            throw std::runtime_error("M1 native save/reopen mismatch");
        focusSelection();
        smokeStage_ = 2;
    }
    const auto mouse = [&](ImVec2 p, int button) {
        testInput_ = [p, button] {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(p.x, p.y);
            if (button >= 0)
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, button != 0);
        };
    };
    if (uiProbe_.stage == 0 && frame >= 40) {
        uiProbe_.id = document_.selection;
        const auto& bounds = document_.scene.transform(document_.scene.find(uiProbe_.id)).bounds;
        const auto projected = sceneView_.viewProjection * glm::vec4((bounds.min + bounds.max) * .5f, 1);
        uiProbe_.mouse = {viewportPosition_.x + (projected.x / projected.w * .5f + .5f) * viewportSize_.x,
                          viewportPosition_.y + (.5f - projected.y / projected.w * .5f) * viewportSize_.y};
        document_.selection = {};
        mouse(uiProbe_.mouse, 1);
        uiProbe_.stage = 1;
    } else if (uiProbe_.stage == 1 && frame >= 41) {
        mouse(uiProbe_.mouse, 0);
        uiProbe_.stage = 2;
    } else if (uiProbe_.stage == 2 && frame >= 43) {
        if (document_.selection != uiProbe_.id)
            throw std::runtime_error("M1 viewport click did not select the cube");
        uiProbe_.picked = true;
        uiProbe_.stage = 3;
    } else if (uiProbe_.stage == 3 && frame >= 62) {
        uiProbe_.originalX = document_.scene.transform(document_.scene.find(uiProbe_.id)).local.position.x;
        uiProbe_.history = document_.undoCount();
        uiProbe_.mouse = {positionInputMin_.x + (positionInputMax_.x - positionInputMin_.x) / 6,
                          (positionInputMin_.y + positionInputMax_.y) * .5f};
        mouse(uiProbe_.mouse, 1);
        uiProbe_.stage = 4;
    } else if (uiProbe_.stage == 4 && frame >= 63) {
        mouse({uiProbe_.mouse.x + 40, uiProbe_.mouse.y}, -1);
        uiProbe_.stage = 5;
    } else if (uiProbe_.stage == 5 && frame >= 64) {
        mouse({uiProbe_.mouse.x + 80, uiProbe_.mouse.y}, -1);
        uiProbe_.stage = 6;
    } else if (uiProbe_.stage == 6 && frame >= 65) {
        mouse({uiProbe_.mouse.x + 80, uiProbe_.mouse.y}, 0);
        uiProbe_.stage = 7;
    } else if (uiProbe_.stage == 7 && frame >= 68) {
        const float changed = document_.scene.transform(document_.scene.find(uiProbe_.id)).local.position.x;
        if (std::abs(changed - uiProbe_.originalX) < .1f || document_.undoCount() != uiProbe_.history + 1)
            throw std::runtime_error("M1 inspector drag did not produce exactly one Undo command");
        document_.undo();
        if (std::abs(document_.scene.transform(document_.scene.find(uiProbe_.id)).local.position.x -
                     uiProbe_.originalX) > .0001f)
            throw std::runtime_error("M1 inspector drag Undo failed");
        document_.redo();
        if (std::abs(document_.scene.transform(document_.scene.find(uiProbe_.id)).local.position.x - changed) > .0001f)
            throw std::runtime_error("M1 inspector drag Redo failed");
        document_.undo();
        uiProbe_.dragged = true;
        uiProbe_.stage = 8;
        sceneSmokePassed_ = true;
    }
}
} // namespace proto
