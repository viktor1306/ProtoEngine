# ProtoEngine — Vulkan 1.3 Rendering Engine

## Огляд Архітектури

**ProtoEngine** — це сучасний, високопродуктивний рушій рендерингу, побудований на **Vulkan 1.3/1.4**. Він відмовляється від застарілих концепцій (таких як RenderPasses) на користь **Dynamic Rendering** та **Bindless Resources**, що забезпечує максимальну гнучкість та продуктивність.

---

## Структура Директорій

```
ProtoEngine/
├── src/
│   ├── core/               Платформа: Window, InputManager, Math, ShaderHotReloader, Timer
│   ├── gfx/
│   │   ├── core/           Vulkan ядро: VulkanContext, Swapchain
│   │   ├── resources/      GPU ресурси: Buffer, Mesh, Texture, GeometryManager (з дефрагментацією)
│   │   ├── rendering/      Рендеринг: Renderer, Pipeline, BindlessSystem, RenderPassProvider
│   │   ├── sync/           Синхронізація: CommandManager, SyncManager
│   │   └── *.hpp           Forwarding headers (зворотна сумісність)
│   ├── scene/              Сцена: Camera, Frustum
│   ├── ui/                 UI: ImGuiManager, TextRenderer, FontSDF
│   └── world/              Світ: Chunk, ChunkManager, ChunkStorage, LODController, Raycast
├── shaders/                GLSL шейдери (.vert, .frag)
├── bin/
│   ├── shaders/            Скомпільовані SPIR-V шейдери
│   ├── fonts/              TrueType шрифти
│   └── engine.exe          Виконуваний файл
└── Makefile
```

---

## Ключові Технічні Особливості

### 1. Vulkan 1.3 Core
- **Dynamic Rendering**: Жодних `VkRenderPass` або `VkFramebuffer`. Рендеринг безпосередньо в зображення через `vkCmdBeginRendering`.
- **Synchronization 2**: Спрощені та ефективніші бар'єри пам'яті (`VkImageMemoryBarrier2`, `VkBufferMemoryBarrier2`, `vkQueueSubmit2`).
- **Buffer Device Address (BDA)**: GPU-вказівники для прямого доступу до буферів (підготовка до Ray Tracing).

### 2. Bindless Architecture
- Доступ до текстур, буферів об'єктів та палітр кольорів через глобальні масиви в шейдерах (`textures[]`, `PaletteBuffer`).
- Знімає обмеження на кількість прив'язаних ресурсів (дозволяє малювати всю сцену за один виклик).
- Об'єкти оновлюють свої дані в глобальному SSBO (Shader Storage Buffer Object).

### 3. Управління Пам'яттю та Геометрія
- **VMA (Vulkan Memory Allocator)**: Ефективне виділення пам'яті GPU для всіх ресурсів (буфери, текстури, shadow image).
- **Monolithic Geometry (GeometryManager)**: Вся геометрія живе в одному масивному Vertex/Index буфері з вбудованим механізмом **вільних блоків (Free-lists)** для дефрагментації пам'яті під час швидкого завантаження/вивантаження чанків.
- **Multithreading**: Багатопотокова генерація чанків за допомогою C++20 `std::jthread`. Асинхронна побудова сіток та фонове оновлення буферів без заїкань (stuttering).

### 4. Підсистеми Синхронізації
- **CommandManager**: RAII керування `VkCommandBuffer` per-frame.
- **SyncManager**: Семафори + фенси + `vkQueueSubmit2` + present.

### 5. Рендеринг Проходів
- **RenderPassProvider**: Інкапсулює `vkCmdBeginRendering` для Shadow Pass та Main Pass.
- **Shadow Mapping**: Напрямлене світло з PCF та Front-Face Culling. Shadow image через VMA.

### 6. Воксельний Світ та LOD
- **Greedy Meshing**: Спеціальний алгоритм стиснення геометрії чанків, який зливає сусідні однакові грані у великі прямокутники, драматично зменшуючи кількість полігонів.
- **Closed Chunk Meshes**: Універсальна відмова від між-чанкового culling. Кожен чанк ідеально ізольовано "закриває" власні межі, унеможливлюючи появу T-Junctions (дірок) при стикуванні різних LOD-рівнів (віддалених чанків з низькою деталізацією).
- **Smooth LOD Transitions**: Перемішування чанків з нижчою роздільною здатністю (LOD 1, 2) на льоту, залежно від дистанції до камери.

### 7. Інструменти Розробника
- **Dear ImGui**: Повноцінне інтегроване UI для параметризації рушія (LOD відстані, статус пам'яті, багатопоточність).
- **Shader Hot-Reloading**: Редагування шейдерів в реальному часі без перезапуску.
- **Auto CWD**: Автоматичне встановлення робочої директорії на project root при запуску.

---

## Інструкція зі Збірки

### Вимоги
- **Vulkan SDK 1.3+** (встановлена змінна середовища `VULKAN_SDK`)
- **MinGW-w64** (Make & G++ з підтримкою C++20)
- **MSYS2** (рекомендовано: `C:/msys64/ucrt64`)

### Компіляція
```bash
mingw32-make
```

### Очищення
```bash
mingw32-make clean
```

### Компіляція шейдерів
```bash
mingw32-make shaders
```

### Запуск
```bash
bin/engine.exe
```
Або запустіть через VS Code (F5) — конфігурація в `.vscode/launch.json`.

### Керування
| Клавіша | Дія |
|---------|-----|
| `W/A/S/D` | Рух камери |
| `ПКМ + Миша` | Огляд |
| `Esc` | Вихід |

---

## Залежності (Vendor)
- `vk_mem_alloc.h` — Vulkan Memory Allocator
- `stb_truetype.h` — TrueType растеризація для SDF шрифтів
