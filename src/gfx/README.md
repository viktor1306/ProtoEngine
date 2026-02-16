# Модуль Graphics (`src/gfx`)

Це серце рушія, що реалізує сучасний конвеєр рендерингу на базі Vulkan 1.3.

## Можливості
- **Vulkan 1.3**: Використовує Dynamic Rendering (без RenderPass/Framebuffer об'єктів), Synchronization 2 та Buffer Device Address (BDA).
- **Bindless Rendering**: Доступ до текстур та буферів через глобальні масиви дескрипторів (`nonuniformEXT`).
- **Управління Пам'яттю**: Працює на базі **Vulkan Memory Allocator (VMA)**.
- **Shadow Mapping**: Напрямлене світло з PCF (Percentage-Closer Filtering) та Front-Face Culling.

## Ключові Компоненти

### `VulkanContext`
- Ініціалізує `VkInstance`, `VkDevice`, `VkQueue` та `VmaAllocator`.
- Вмикає необхідні розширення (`VK_KHR_dynamic_rendering` тощо).

### `Renderer`
- Керує циклом рендерингу, синхронізацією та командними буферами.
- Реалізує `reloadShaders()` для гарячого перезавантаження шейдерів без зупинки.
- Обробляє проходи Dynamic Rendering (Shadow Pass -> Main Pass).

### `BindlessSystem`
- Керує глобальними наборами дескрипторів (Set 1).
- **Object Buffers**: SSBO для даних об'єктів (Model Matrix, Texture ID).
- **Textures**: Глобальний масив семпльованих зображень.
- **Double Buffering**: Перевірка оновлень гарантує безпеку даних для кадрів в обробці (in-flight frames).

### `GeometryManager` та `Mesh`
- **Monolithic Buffers**: Вся геометрія живе в одному величезному Vertex/Index буфері (керованому VMA).
- `Mesh` — це легкий дескриптор (містить зміщення в глобальному буфері).
- Структура `Vertex`: Position (3), Normal (3), Color (3), UV (2). **Вирівнювання 48 байт**.

### `Pipeline`
- Обгортка для створення `VkPipeline`.
- Підтримує Dynamic Rendering (формати кольору/глибини замість RenderPass).
- Конфігурується (Cull Mode, Depth Test, шляхи до шейдерів).
- Автоматизує створення/видалення `VkShaderModule`.

### `Swapchain`
- Керує зображеннями для відображення на екрані та буферами глибини.
- Обробляє зміну розміру вікна та перестворення ланцюжка.
