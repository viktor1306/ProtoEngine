# Локальна перевірка основи

Дата: 2026-09-08. Нижче збережено результати попередніх системних запитів
і діагностичних probes. Після них реалізовано й перевірено каркас M0;
актуальні результати редактора наведено у [звіті M0](M0_REPORT.md).

## Обладнання та система

| Параметр | Результат |
| --- | --- |
| CPU | Intel Core Ultra 7 255H, 16 cores / 16 logical processors за WMI |
| RAM | 63.4 GiB фізичної пам'яті, видимої ОС (`TotalPhysicalMemory`), приблизно 64 GB установленої пам'яті |
| GPU | Intel Arc 140T, integrated GPU за Vulkan |
| Назва, повідомлена драйвером | `Intel(R) Arc(TM) 140T GPU (32GB)` |
| ОС | Windows 11 Pro x64, 10.0.26200 |
| Драйвер | 32.0.101.8508 |
| Поточна роздільність дисплея | 1920×1200 за WMI |

Позначка `(32GB)` у назві адаптера **не є доказом 32 GB окремої VRAM**.
Значення AdapterRAM у WMI також не використовуємо як надійну оцінку доступної
пам'яті інтегрованого GPU. Для роботи потрібен фактичний budget і облік ресурсів.

## Наявний C++-набір

GCC/g++ 16.2.0, CMake 4.4.2, Ninja 1.13.2, GDB 17.2, Make 4.4.1 із w64devkit.
Раніше перевірено збірку й запуск C++20 напряму та через CMake/Ninja.
Новий Python не встановлювався; діагностичні програми компілювалися на C/C++.

## OpenGL — перевірений альтернативний кандидат

Прихований Win32/WGL-контекст успішно створено із запитом **OpenGL 4.5 Core**.

- OpenGL: `4.5.0 - Build 32.0.101.8508`; GLSL: `4.50`.
- Max texture/cubemap size: 16384; array layers: 2048; SSBO bindings: 16.
- GPU timer query: 64 bits.
- Compute-шейдер записав `42` у SSBO; readback повернув `42`.
- DSA-створення depth cube array з 12 faces пройшло; `glGetError = 0`.

Перевірено саме запитаний контекст 4.5, а не максимальну можливу версію драйвера.
Результат показує технічну придатність OpenGL для ефектів, але не його швидкодію.

## Vulkan — поточний архітектурний вибір

Системні `vulkan-1.dll` і `vulkaninfo.exe` уже були присутні.

- Loader / instance version: 1.4.313; device API: **1.4.335**.
- Доступні dynamic rendering, synchronization2, timeline semaphore,
  imageCubeArray, multiDrawIndirect, `VK_KHR_swapchain`, `VK_EXT_memory_budget`.
- Програма з volk/VMA успішно створила device із запитом **Vulkan 1.3**.
- Створено mapped storage buffer, depth cube array image і cube-array view.
- glslang 16.5.0 згенерував SPIR-V для Vulkan 1.3.
- Compute dispatch виконався; synchronization2 та timeline semaphore
  забезпечили завершення; readback повернув **42**.
- Win32 presentation support для вибраної queue family — true.

Перевірений glslang executable залежить від наявного MSVC runtime
(`MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`); на цьому ПК він запустився.
M0 тепер запускає цей preflight при кожному configure. Інший ПК не перевірявся.

У початковій compute-пробі повний raster pass зі swapchain не виконувався.
Він уже перевірений у реалізації M0, включно з GPU readback.

## Залежності

На GCC 16.2 пройшли компіляція, зв'язування й малий smoke check:

- Dear ImGui 1.92.9b: створення й знищення context.
- ImGuizmo: декомпозиція identity matrix із цією версією ImGui.
- cgltf v1.15: parsing мінімального glTF.
- yyjson 0.12.0: parsing і читання поля.
- Vulkan-Headers / volk / VMA: створення device, ресурсів і compute-виконання.

Ці перевірки не є повною перевіркою importer, UI-backend або SDK.
Вихідні файли та shader compiler завантажено лише в окрему тимчасову папку;
пакети не встановлювалися глобально.

## Межі початкових probes та подальша перевірка

- У початковій системній конфігурації validation layer був відсутній.
  M0 використовує локальний layer 1.4.357.0 тільки в Debug: 0 errors/warnings.
- Loader надрукував попередження про пошук layer manifest у registry;
  запити й compute-проба завершилися успішно. Registry не змінювався.
- GLFW + ImGui Vulkan backend + swapchain тепер перевірені як єдина програма M0.
- Є діагностичні таймери й облік VMA для M0; benchmark майбутньої 3D-сцени ще немає.
- Порівняння швидкодії Vulkan та OpenGL **не проводилося**.
- Запуск на іншому/слабшому ПК і Windows 10 не перевірено.

Машинні результати probes: [research/capabilities.json](research/capabilities.json).
