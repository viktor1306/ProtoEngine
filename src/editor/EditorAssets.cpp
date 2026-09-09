#include "editor/Editor.hpp"
#include "editor/FileDialog.hpp"
#include "project/ProjectSession.hpp"
#include "project/AssetFilePlanner.hpp"
#include <algorithm>

namespace proto {
void Editor::importModel() {
    try {
        if (document_.path().empty() && !saveScene())
            return;
        const auto source = modelFileDialog(window_);
        if (!source)
            return;
        document_.endGesture();
        importScene_ = document_.scene.id;
        importInstance_ = true;
        importJob_.start(document_.workspace(), *source);
        status_ = "Імпорт: " + utf8(source->filename().wstring());
    } catch (const std::exception& e) {
        showError(e);
    }
}
void Editor::pollImport() {
    if (!importJob_.busy() || document_.gestureActive())
        return;
    try {
        const auto result = importJob_.take();
        if (!result)
            return;
        if (project_) {
            executeFilePlan(enrollProjectFiles(project_->root()));
            refreshProjectFiles();
        }
        if (document_.scene.id != importScene_) {
            status_ = "Імпорт завершено в папці ресурсів попередньої сцени.";
            return;
        }
        document_.endGesture();
        if (importInstance_) {
            document_.instantiate(result);
            focusSelection();
        } else {
            document_.scene.assets->publish(result);
            document_.scene.assetsChanged();
        }
        status_ = result->name + (result->fromCache ? " — готово з кешу" : " — імпортовано");
    } catch (const std::exception& e) {
        status_ = "Імпорт не завершено";
        showError(e);
    }
}
void Editor::materialInspector(const MeshRenderer& renderer) {
    const auto& mesh = *document_.scene.assets->meshes.at(renderer.mesh);
    if (mesh.parts.empty())
        return;
    if (materialMesh_ != renderer.mesh) {
        materialMesh_ = renderer.mesh;
        materialSlot_ = 0;
    }
    materialSlot_ = std::clamp(materialSlot_, 0, static_cast<int>(mesh.parts.size()) - 1);
    const auto materialId = [&](size_t slot) {
        return renderer.materials.empty() ? mesh.parts[slot].material : renderer.materials.at(slot);
    };
    auto material = document_.scene.assets->materials.at(materialId(static_cast<size_t>(materialSlot_)));
    ImGui::SeparatorText("Матеріал");
    if (ImGui::BeginCombo("##MaterialSlot", material->name.c_str())) {
        for (size_t slot = 0; slot < mesh.parts.size(); ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            if (ImGui::Selectable(document_.scene.assets->materials.at(materialId(slot))->name.c_str(),
                                  slot == static_cast<size_t>(materialSlot_))) {
                try {
                    document_.endGesture();
                    materialSlot_ = static_cast<int>(slot);
                } catch (const std::exception& e) {
                    showError(e);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%s · %s", material->values.unlit ? "Unlit" : "PBR", material->values.mask ? "MASK" : "OPAQUE");
    if (material->values.doubleSided)
        ImGui::TextDisabled("Двосторонній матеріал");
    ImGui::TextWrapped("Зміни спільні для всіх об’єктів із цим матеріалом і зберігаються після редагування.");
    auto values = material->values;
    const auto gesture = [&](bool changed) {
        try {
            if (ImGui::IsItemActivated())
                document_.beginMaterialGesture(material->id);
            if (changed)
                document_.previewMaterial(material->id, values);
            if (ImGui::IsItemDeactivated())
                document_.endGesture();
        } catch (const std::exception& e) {
            showError(e);
        }
    };
    ImGui::BeginDisabled(importJob_.busy());
    ImGui::TextDisabled("Базовий колір");
    gesture(ImGui::ColorEdit3("##BaseColor", &values.baseColor.r));
    if (!values.unlit) {
        ImGui::TextDisabled("Металевість");
        gesture(ImGui::SliderFloat("##Metallic", &values.metallic, 0, 1));
        ImGui::TextDisabled("Шорсткість");
        gesture(ImGui::SliderFloat("##Roughness", &values.roughness, 0, 1));
        if (material->textures[2].texture) {
            ImGui::TextDisabled("Сила normal map");
            gesture(ImGui::SliderFloat("##NormalScale", &values.normalScale, 0, 4));
        }
    }
    ImGui::EndDisabled();
    constexpr const char* roles[]{"Base color", "Metallic / roughness", "Normal", "Occlusion", "Emissive"};
    for (size_t i = 0; i < 5; ++i)
        if (material->textures[i].texture) {
            const auto& image = document_.scene.assets->textures.at(material->textures[i].texture)->pixels;
            ImGui::TextDisabled("%s: %u × %u", roles[i], image->mips[0].width, image->mips[0].height);
        }
}
} // namespace proto
