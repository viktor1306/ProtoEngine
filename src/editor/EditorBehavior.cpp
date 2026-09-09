#include "editor/Editor.hpp"
#include "behavior/BehaviorSchema.hpp"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace proto {
namespace {
constexpr ImVec4 warning{1.0f, .67f, .28f, 1.0f};
constexpr ImVec4 error{1.0f, .40f, .34f, 1.0f};

const char* propertyTypeName(sdk::PropertyType type) {
    switch (type) {
    case sdk::PropertyType::Bool:
        return "bool";
    case sdk::PropertyType::Integer:
        return "integer";
    case sdk::PropertyType::Float:
        return "float";
    case sdk::PropertyType::String:
        return "string";
    case sdk::PropertyType::Vec3:
        return "vec3";
    case sdk::PropertyType::Color:
        return "color";
    case sdk::PropertyType::EntityRef:
        return "entityRef";
    case sdk::PropertyType::AssetRef:
        return "assetRef";
    }
    return "unknown";
}

const char* valueTypeName(const sdk::PropertyValue& value) {
    return std::visit(
        [](const auto& item) -> const char* {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return "bool";
            else if constexpr (std::is_same_v<T, int64_t>)
                return "integer";
            else if constexpr (std::is_same_v<T, double>)
                return "float";
            else if constexpr (std::is_same_v<T, std::string>)
                return "string";
            else if constexpr (std::is_same_v<T, sdk::Vec3>)
                return "vec3";
            else if constexpr (std::is_same_v<T, sdk::Color>)
                return "color";
            else if constexpr (std::is_same_v<T, sdk::EntityRef>)
                return "entityRef";
            else
                return "assetRef";
        },
        value);
}

bool compatible(sdk::PropertyType type, const sdk::PropertyValue& value) {
    switch (type) {
    case sdk::PropertyType::Bool:
        return std::holds_alternative<bool>(value);
    case sdk::PropertyType::Integer:
        return std::holds_alternative<int64_t>(value);
    case sdk::PropertyType::Float:
        return std::holds_alternative<double>(value) || std::holds_alternative<int64_t>(value);
    case sdk::PropertyType::String:
        return std::holds_alternative<std::string>(value);
    case sdk::PropertyType::Vec3:
        return std::holds_alternative<sdk::Vec3>(value);
    case sdk::PropertyType::Color:
        return std::holds_alternative<sdk::Color>(value);
    case sdk::PropertyType::EntityRef:
        return std::holds_alternative<sdk::EntityRef>(value);
    case sdk::PropertyType::AssetRef:
        return std::holds_alternative<sdk::AssetRef>(value);
    }
    return false;
}

int64_t integerBound(double value) {
    if (value <= static_cast<double>(std::numeric_limits<int64_t>::min()))
        return std::numeric_limits<int64_t>::min();
    if (value >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(value);
}

std::string propertyLabel(const sdk::PropertyDescriptor& descriptor) {
    return descriptor.label.empty() ? descriptor.name : descriptor.label;
}

// Draw one value in the schema type. The caller owns the copy, so a missing
// property is only inserted into the authored record after the user changes
// this widget.
bool editProperty(const sdk::PropertyDescriptor& descriptor, sdk::PropertyValue& value) {
    const auto label = propertyLabel(descriptor);
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    switch (descriptor.type) {
    case sdk::PropertyType::Bool: {
        auto current = std::get<bool>(value);
        if (!ImGui::Checkbox(("##" + descriptor.name).c_str(), &current))
            return false;
        value = current;
        return true;
    }
    case sdk::PropertyType::Integer: {
        auto current = std::get<int64_t>(value);
        const int64_t minimum = descriptor.min ? integerBound(*descriptor.min) : std::numeric_limits<int64_t>::min();
        const int64_t maximum = descriptor.max ? integerBound(*descriptor.max) : std::numeric_limits<int64_t>::max();
        const int64_t* minimumPtr = descriptor.min ? &minimum : nullptr;
        const int64_t* maximumPtr = descriptor.max ? &maximum : nullptr;
        if (!ImGui::DragScalar(("##" + descriptor.name).c_str(), ImGuiDataType_S64, &current, 1.0f, minimumPtr,
                               maximumPtr, "%lld"))
            return false;
        value = current;
        return true;
    }
    case sdk::PropertyType::Float: {
        const auto asDouble = [](const sdk::PropertyValue& item) {
            return std::holds_alternative<int64_t>(item) ? static_cast<double>(std::get<int64_t>(item))
                                                         : std::get<double>(item);
        };
        auto current = asDouble(value);
        const double minimum = descriptor.min.value_or(-std::numeric_limits<double>::max());
        const double maximum = descriptor.max.value_or(std::numeric_limits<double>::max());
        const double* minimumPtr = descriptor.min ? &minimum : nullptr;
        const double* maximumPtr = descriptor.max ? &maximum : nullptr;
        if (!ImGui::DragScalar(("##" + descriptor.name).c_str(), ImGuiDataType_Double, &current, .05f, minimumPtr,
                               maximumPtr, "%.4f"))
            return false;
        value = current;
        return true;
    }
    case sdk::PropertyType::String: {
        const auto& source = std::get<std::string>(value);
        constexpr size_t maxStringBytes = 1024u * 1024u;
        if (source.size() > maxStringBytes) {
            ImGui::TextColored(error, "Значення завелике для редактора (%zu байт)", source.size());
            return false;
        }
        auto current = source;
        if (!ImGui::InputText(("##" + descriptor.name).c_str(), &current))
            return false;
        value = std::move(current);
        return true;
    }
    case sdk::PropertyType::Vec3: {
        auto current = std::get<sdk::Vec3>(value);
        float data[3]{current.x, current.y, current.z};
        if (!ImGui::DragFloat3(("##" + descriptor.name).c_str(), data, .02f, 0, 0, "%.4f"))
            return false;
        value = sdk::Vec3{data[0], data[1], data[2]};
        return true;
    }
    case sdk::PropertyType::Color: {
        auto current = std::get<sdk::Color>(value);
        float data[4]{current.r, current.g, current.b, current.a};
        if (!ImGui::ColorEdit4(("##" + descriptor.name).c_str(), data, ImGuiColorEditFlags_Float))
            return false;
        value = sdk::Color{data[0], data[1], data[2], data[3]};
        return true;
    }
    case sdk::PropertyType::EntityRef: {
        const auto current = std::get<sdk::EntityRef>(value);
        std::array<char, 80> buffer{};
        const auto text = current.id ? current.id.string() : std::string{};
        std::memcpy(buffer.data(), text.data(), std::min(text.size(), buffer.size() - 1));
        const bool changed = ImGui::InputText(("##" + descriptor.name).c_str(), buffer.data(), buffer.size());
        if (!changed)
            return false;
        try {
            value = sdk::EntityRef{buffer[0] ? EntityId::parse(buffer.data()) : EntityId{}};
            return true;
        } catch (const std::exception&) {
            ImGui::TextColored(error, "Некоректний EntityId");
            return false;
        }
    }
    case sdk::PropertyType::AssetRef: {
        const auto current = std::get<sdk::AssetRef>(value);
        std::array<char, 80> buffer{};
        const auto text = current.id ? current.id.string() : std::string{};
        std::memcpy(buffer.data(), text.data(), std::min(text.size(), buffer.size() - 1));
        const bool changed = ImGui::InputText(("##" + descriptor.name).c_str(), buffer.data(), buffer.size());
        if (!changed)
            return false;
        try {
            value = sdk::AssetRef{buffer[0] ? AssetId::parse(buffer.data()) : AssetId{}};
            return true;
        } catch (const std::exception&) {
            ImGui::TextColored(error, "Некоректний AssetId");
            return false;
        }
    }
    }
    return false;
}

} // namespace

void Editor::behaviorInspector() {
    ImGui::SeparatorText("Поведінки");
    const auto handle = document_.scene.find(document_.selection);
    if (!handle)
        return;

    auto record = document_.scene.record(handle);
    const auto* schema = behaviorSchema();
    const bool blocked = playBusy();
    ImGui::BeginDisabled(blocked);

    for (size_t index = 0; index < record.behaviors.size();) {
        auto& binding = record.behaviors[index];
        const auto* descriptor = schema ? schema->find(binding.type) : nullptr;
        const std::string title = descriptor ? descriptor->name : "Невідомий тип · " + binding.type.string();
        ImGui::PushID(binding.id.string().c_str());
        const bool expanded = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        bool remove = false;
        if (expanded) {
            bool enabled = binding.enabled;
            if (ImGui::Checkbox("Увімкнено", &enabled)) {
                binding.enabled = enabled;
                try {
                    document_.edit(record, "Зміна стану поведінки");
                } catch (const std::exception& errorValue) {
                    showError(errorValue);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Видалити поведінку"))
                remove = true;
            if (descriptor) {
                for (const auto& property : descriptor->properties) {
                    ImGui::PushID(property.name.c_str());
                    const auto found = binding.properties.find(property.name);
                    const bool stored = found != binding.properties.end();
                    if (stored && !compatible(property.type, found->second)) {
                        ImGui::TextColored(error, "%s · очікується %s", propertyLabel(property).c_str(),
                                           propertyTypeName(property.type));
                        ImGui::TextDisabled("Збережено несумісне значення типу %s; воно не змінене.",
                                            valueTypeName(found->second));
                    } else {
                        auto value = stored ? found->second : property.defaultValue;
                        if (!stored)
                            ImGui::TextDisabled("За замовчуванням; буде збережено після зміни");
                        ImGui::BeginDisabled(blocked);
                        const bool changed = editProperty(property, value);
                        ImGui::EndDisabled();
                        if (changed)
                            binding.properties[property.name] = std::move(value);
                        propertyGesture(changed, record);
                    }
                    ImGui::PopID();
                }
                std::unordered_set<std::string_view> known;
                for (const auto& property : descriptor->properties)
                    known.insert(property.name);
                for (const auto& [name, value] : binding.properties)
                    if (!known.contains(name)) {
                        ImGui::TextColored(warning, "Невідома властивість: %s (%s)", name.c_str(),
                                           valueTypeName(value));
                        ImGui::TextDisabled("Значення збережено для сумісності.");
                    }
            } else {
                ImGui::TextColored(warning, "Схема типу недоступна; дані збережено без змін.");
                ImGui::TextDisabled("ID типу: %s · Властивостей: %zu", binding.type.string().c_str(),
                                    binding.properties.size());
            }
        }
        ImGui::PopID();
        if (remove) {
            record.behaviors.erase(record.behaviors.begin() + static_cast<std::ptrdiff_t>(index));
            try {
                document_.edit(record, "Видалення поведінки");
            } catch (const std::exception& errorValue) {
                showError(errorValue);
            }
            break;
        }
        ++index;
    }

    if (!schema) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("Схема поведінок з’явиться після успішної збірки C++.");
        return;
    }
    if (schema->types.empty()) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("У зібраному проєкті немає зареєстрованих поведінок.");
        return;
    }

    static BehaviorTypeId addType;
    if (!addType || !schema->find(addType))
        addType = schema->types.front().id;
    const auto* selected = schema->find(addType);
    if (ImGui::BeginCombo("Додати поведінку", selected ? selected->name.c_str() : "Оберіть тип")) {
        for (const auto& type : schema->types) {
            ImGui::PushID(type.id.string().c_str());
            const bool isSelected = type.id == addType;
            if (ImGui::Selectable(type.name.c_str(), isSelected))
                addType = type.id;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Додати поведінку")) {
        auto changed = document_.scene.record(handle);
        changed.behaviors.push_back({BehaviorBindingId::create(), addType, true, {}});
        try {
            document_.edit(changed, "Додати поведінку");
        } catch (const std::exception& errorValue) {
            showError(errorValue);
        }
    }
    ImGui::EndDisabled();
}

void Editor::behaviorStringProbe() {
    if (!stringProbe_.enabled || stringProbe_.passed)
        return;
    ImGui::SetNextWindowPos(ImVec2(24, 96), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("M5 String input probe###M5StringInputProbe", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::SetWindowFocus();
    ImGui::TextDisabled("Діагностика String-властивості");
    sdk::PropertyDescriptor descriptor{"m5_string_probe", sdk::PropertyType::String, std::string{}, {}, {},
                                       "Тестовий текст"};
    sdk::PropertyValue value = stringProbe_.value;
    if (editProperty(descriptor, value))
        stringProbe_.value = std::get<std::string>(value);
    stringProbe_.inputMin = ImGui::GetItemRectMin();
    stringProbe_.inputMax = ImGui::GetItemRectMax();
    ImGui::TextDisabled("Значення: %s", stringProbe_.value.c_str());
    ImGui::End();
}
} // namespace proto
