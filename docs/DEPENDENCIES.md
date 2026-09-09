# Proto Engine v0.1 — залежності

Перевірено джерела та версії 2026-09-08. M0 інтегрує GLFW, Dear ImGui,
Vulkan-Headers, volk та VMA; M1 додає GLM і yyjson, M2 — cgltf, stb_image та
MikkTSpace. M4 реалізує маніпулятори власною математикою GLM та малюванням ImGui;
ImGuizmo не підключений. Використовуються готові вихідні файли
й переносний shader compiler; **новий Python не потрібний**.

## Набір

| Залежність | Версія / джерело | Призначення | Ліцензія / повідомлення |
| --- | --- | --- | --- |
| GLFW | [3.5.1](https://github.com/glfw/glfw/releases/tag/3.5.1) | Вікно, введення, Vulkan surface | [zlib](https://www.glfw.org/license.html) |
| Dear ImGui | [v1.92.9b-docking](https://github.com/ocornut/imgui/tree/v1.92.9b-docking) | Редактор і невеликий Runtime UI | [MIT](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/LICENSE.txt) |
| ImGuizmo | [18cef5e](https://github.com/CedricGuillemet/ImGuizmo/tree/18cef5e031d8c6973d80284c67f60549fafd78c1) | Маніпулятори трансформацій, лише редактор | [MIT](https://github.com/CedricGuillemet/ImGuizmo/blob/18cef5e031d8c6973d80284c67f60549fafd78c1/LICENSE) |
| GLM | [1.0.3](https://github.com/g-truc/glm/releases/tag/1.0.3) | Внутрішня математика | MIT-варіант [подвійної ліцензії](https://github.com/g-truc/glm/blob/1.0.3/copying.txt) |
| cgltf | [v1.15](https://github.com/jkuhlmann/cgltf/releases/tag/v1.15) | glTF/GLB parsing, лише importer | [MIT](https://github.com/jkuhlmann/cgltf/blob/v1.15/LICENSE) |
| stb_image | [2c980bb](https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20), header 2.30 | PNG/JPEG, лише importer | MIT-варіант [ліцензії у файлі](https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h) |
| MikkTSpace | [3e895b4](https://github.com/mmikk/MikkTSpace/tree/3e895b49d05ea07e4c2133156cfa94369e19e409) | Тангенти, лише importer | [zlib-style notice](https://github.com/mmikk/MikkTSpace/blob/3e895b49d05ea07e4c2133156cfa94369e19e409/mikktspace.h) |
| yyjson | [0.12.0](https://github.com/ibireme/yyjson/releases/tag/0.12.0) | Версійовані файли й metadata | [MIT](https://github.com/ibireme/yyjson/blob/0.12.0/LICENSE) |
| Vulkan-Headers | [SDK 1.4.357.0](https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.357.0) | Заголовки; runtime-ціль API 1.3 | [Apache-2.0 OR MIT / повідомлення файлів](https://github.com/KhronosGroup/Vulkan-Headers/blob/vulkan-sdk-1.4.357.0/LICENSE.md) |
| volk | [SDK 1.4.357.0](https://github.com/zeux/volk/tree/vulkan-sdk-1.4.357.0) | Завантаження Vulkan entry points | [MIT](https://github.com/zeux/volk/blob/vulkan-sdk-1.4.357.0/LICENSE.md) |
| Vulkan Memory Allocator | [v3.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/releases/tag/v3.4.0) | GPU suballocation і бюджет | [MIT](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/v3.4.0/LICENSE.txt) |
| glslang | [16.5.0](https://github.com/KhronosGroup/glslang/releases/tag/16.5.0) | GLSL → SPIR-V при збірці | [Повідомлення upstream](https://github.com/KhronosGroup/glslang/blob/16.5.0/LICENSE.txt) |

Версія Vulkan-Headers не підвищує автоматично runtime-вимогу до Vulkan 1.4.
У програмі запитується 1.3 та перевіряються потрібні features/extensions.

Вікна й меню використовують системний шрифт Windows з українськими символами;
файл шрифту не копіюється до пакета. Перевірка Cyrillic, DPI і clipboard входить у M0.

## Незмінні revision

| Залежність | Commit |
| --- | --- |
| GLFW | `d9d6f0f1f967807ffade6598ea9a631ebaf37a56` |
| Dear ImGui docking | `b48d1afbe8ee8b238e2961dc363a949dd7304e23` |
| ImGuizmo | `18cef5e031d8c6973d80284c67f60549fafd78c1` |
| GLM | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` |
| cgltf | `360db1a95480fe102ae9c69b27c5d101167ff5ba` |
| stb | `2c980bb59875b0d32144a71867fbdebb2f77cd20` |
| MikkTSpace | `3e895b49d05ea07e4c2133156cfa94369e19e409` |
| yyjson | `8b4a38dc994a110abaec8a400615567bd996105f` |
| Vulkan-Headers | `e3b1eec08173d6b825cd3ac88c885a63b621504a` |
| volk | `776893306c5d3b22b6185b5d4a258b81d94572bf` |
| VMA | `3aa921224c154a0d2c43912bc88e1c42ce1f7607` |
| glslang | `a8d28bd082bff18ffbe80996e922b012f915cf07` |

Revision і SHA-256 активних архівів зафіксовані в [build lock](../dependencies.lock.json).
Revision відкладених бібліотек також збережені в ньому; їхні архіви ще не інтегровані.
Не використовуємо плаваючі `master`, `latest` або тег без перевірки відповідного commit.

M2 використовує `stb_image_write` з того самого pinned stb лише в генераторі
оригінальних тестових ресурсів. Він не входить до `ProtoEditor.exe`.
Для MASK glslang запускається з `--discard-is-terminate`, щоб не вимагати
неувімкнену optional feature `shaderDemoteToHelperInvocation`.

Одноразовий dev-formatter: native clang-format 20.1.8, витягнутий із
[пакета maintainer на PyPI](https://pypi.org/project/clang-format/20.1.8/).
Архів 1 414 174 bytes, SHA-256
`346ac8cab571eaba4d6b89dfa30fdbbc512db82a66ab0eeb1763cacc5977e325`.
У `.cache/tools/clang-format-20.1.8` збережено лише executable, provenance та
license; Python, pip і wheel-пакет не встановлювали. Formatter не є залежністю
збірки чи редактора. Конфігурація форматування — `.clang-format`.

Перевірений переносний shader compiler:

- Файл: `glslang-16.5.0-windows-x86_64-release.zip`, 13 582 336 байтів.
- SHA-256: `06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce`.
- Офіційний [release asset](https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip).
- Запуск `glslang.exe` і компіляція compute-шейдера для Vulkan 1.3 уже пройшли.

Аудит import table цього executable виявив `MSVCP140.dll`, `VCRUNTIME140.dll`
та `VCRUNTIME140_1.dll`, окрім системного UCRT. На поточному ПК вони доступні.
Це передумова інструмента для авторської машини, а не залежність готової гри.
M0 перевіряє запуск shader compiler під час configure; до комплектування SDK
не можна вважати цей exe самодостатнім
на довільному чистому ПК. Поточну систему й Python через це не змінювали.

Повний Vulkan SDK не є вимогою для користувача готової програми. Debug
використовує локальний validation-набір **1.4.357.0**. Його джерело — підписаний
LunarG Windows x64 пакет (287 971 024 байти), отриманий за фіксованим URL із lock.
SHA-256 пакета: `81f474711e9042f4cd22b31b2f7a8870db2e428b21586fb43dd80150be97310d`.

[Setup-Validation.ps1](../scripts/Setup-Validation.ps1) перевіряє hash і підпис,
виконує офіційний `copy_only=1` у тимчасовому локальному каталозі, залишає лише
DLL, JSON manifest і notice. Потім видаляє тимчасовий пакет та витягнуті файли.
DLL/JSON мають власні SHA-256 у lock. Системні PATH, registry і Python не змінюються.
Validation DLL потребує того самого наявного MSVC runtime, що й glslang.
Офіційний опис режиму: [LunarG — unattended installation](https://vulkan.lunarg.com/doc/view/1.4.357.0/windows/getting_started.html).

## Межі залежностей

- cgltf, stb_image, MikkTSpace та ImGuizmo не потрапляють у звичайний Player.
- Shader compiler використовується інструментами збірки; Player читає SPIR-V.
- yyjson працює при завантаженні/збереженні, не обходиться в циклі кожного об'єкта.
- volk завантажує системний loader і device entry points; генератор Python
  не запускається, бо upstream уже містить потрібні C/header-файли.
- GLFW збирається без прикладів, тестових програм і документації.
- Dear ImGui та його backend беруться з одного revision. Зовнішній GUI-фреймворк
  або веб-рушій для редактора не потрібний.

## Що перевірено

На наявному GCC 16.2 пройшов малий headless probe: ImGui context,
зв'язування ImGuizmo та декомпозиція identity matrix, cgltf parse, yyjson parse.
Інший probe зв'язав Vulkan-Headers, volk і VMA, створив пристрій API 1.3,
виділив buffer/image та виконав SPIR-V compute-шейдер на GPU.

GLFW surface, ImGui Vulkan renderer і CMake Debug/Release тепер перевірені
разом у M0. M1 перевірив GLM-трансформації/камеру та yyjson save/load.
MikkTSpace на моделях, gizmo та CMake-збірка майбутнього SDK ще попереду.
Оригінальні notices копіюються до `bin/licenses`; додаткові
повідомлення інструментів і MinGW runtime — у [third_party](../third_party/NOTICES.md).
